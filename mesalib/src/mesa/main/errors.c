/**
 * \file errors.c
 * Mesa debugging and error handling functions.
 */

/*
 * Mesa 3-D graphics library
 *
 * Copyright (C) 1999-2007  Brian Paul   All Rights Reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included
 * in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
 * OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR
 * OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
 * ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
 * OTHER DEALINGS IN THE SOFTWARE.
 */


#include "errors.h"
#include "enums.h"
#include "imports.h"
#include "context.h"
#include "dispatch.h"
#include "hash.h"
#include "mtypes.h"
#include "version.h"
#include "hash_table.h"

static mtx_t DynamicIDMutex = _MTX_INITIALIZER_NP;
static GLuint NextDynamicID = 1;

/**
 * A namespace element.
 */
struct gl_debug_element
{
   struct simple_node link;

   GLuint ID;
   /* at which severity levels (mesa_debug_severity) is the message enabled */
   GLbitfield State;
};

struct gl_debug_namespace
{
   struct simple_node Elements;
   GLbitfield DefaultState;
};

struct gl_debug_group {
   struct gl_debug_namespace Namespaces[MESA_DEBUG_SOURCE_COUNT][MESA_DEBUG_TYPE_COUNT];
};

/**
 * An error, warning, or other piece of debug information for an application
 * to consume via GL_ARB_debug_output/GL_KHR_debug.
 */
struct gl_debug_message
{
   enum mesa_debug_source source;
   enum mesa_debug_type type;
   GLuint id;
   enum mesa_debug_severity severity;
   GLsizei length;
   GLcharARB *message;
};

/**
 * Debug message log.  It works like a ring buffer.
 */
struct gl_debug_log {
   struct gl_debug_message Messages[MAX_DEBUG_LOGGED_MESSAGES];
   GLint NextMessage;
   GLint NumMessages;
};

struct gl_debug_state
{
   GLDEBUGPROC Callback;
   const void *CallbackData;
   GLboolean SyncOutput;
   GLboolean DebugOutput;

   struct gl_debug_group *Groups[MAX_DEBUG_GROUP_STACK_DEPTH];
   struct gl_debug_message GroupMessages[MAX_DEBUG_GROUP_STACK_DEPTH];
   GLint GroupStackDepth;

   struct gl_debug_log Log;
};

static char out_of_memory[] = "Debugging error: out of memory";

static const GLenum debug_source_enums[] = {
   GL_DEBUG_SOURCE_API,
   GL_DEBUG_SOURCE_WINDOW_SYSTEM,
   GL_DEBUG_SOURCE_SHADER_COMPILER,
   GL_DEBUG_SOURCE_THIRD_PARTY,
   GL_DEBUG_SOURCE_APPLICATION,
   GL_DEBUG_SOURCE_OTHER,
};

static const GLenum debug_type_enums[] = {
   GL_DEBUG_TYPE_ERROR,
   GL_DEBUG_TYPE_DEPRECATED_BEHAVIOR,
   GL_DEBUG_TYPE_UNDEFINED_BEHAVIOR,
   GL_DEBUG_TYPE_PORTABILITY,
   GL_DEBUG_TYPE_PERFORMANCE,
   GL_DEBUG_TYPE_OTHER,
   GL_DEBUG_TYPE_MARKER,
   GL_DEBUG_TYPE_PUSH_GROUP,
   GL_DEBUG_TYPE_POP_GROUP,
};

static const GLenum debug_severity_enums[] = {
   GL_DEBUG_SEVERITY_LOW,
   GL_DEBUG_SEVERITY_MEDIUM,
   GL_DEBUG_SEVERITY_HIGH,
   GL_DEBUG_SEVERITY_NOTIFICATION,
};


static enum mesa_debug_source
gl_enum_to_debug_source(GLenum e)
{
   int i;

   for (i = 0; i < Elements(debug_source_enums); i++) {
      if (debug_source_enums[i] == e)
         break;
   }
   return i;
}

static enum mesa_debug_type
gl_enum_to_debug_type(GLenum e)
{
   int i;

   for (i = 0; i < Elements(debug_type_enums); i++) {
      if (debug_type_enums[i] == e)
         break;
   }
   return i;
}

static enum mesa_debug_severity
gl_enum_to_debug_severity(GLenum e)
{
   int i;

   for (i = 0; i < Elements(debug_severity_enums); i++) {
      if (debug_severity_enums[i] == e)
         break;
   }
   return i;
}


/**
 * Handles generating a GL_ARB_debug_output message ID generated by the GL or
 * GLSL compiler.
 *
 * The GL API has this "ID" mechanism, where the intention is to allow a
 * client to filter in/out messages based on source, type, and ID.  Of course,
 * building a giant enum list of all debug output messages that Mesa might
 * generate is ridiculous, so instead we have our caller pass us a pointer to
 * static storage where the ID should get stored.  This ID will be shared
 * across all contexts for that message (which seems like a desirable
 * property, even if it's not expected by the spec), but note that it won't be
 * the same between executions if messages aren't generated in the same order.
 */
static void
debug_get_id(GLuint *id)
{
   if (!(*id)) {
      mtx_lock(&DynamicIDMutex);
      if (!(*id))
         *id = NextDynamicID++;
      mtx_unlock(&DynamicIDMutex);
   }
}

static void
debug_message_clear(struct gl_debug_message *msg)
{
   if (msg->message != (char*)out_of_memory)
      free(msg->message);
   msg->message = NULL;
   msg->length = 0;
}

static void
debug_message_store(struct gl_debug_message *msg,
                    enum mesa_debug_source source,
                    enum mesa_debug_type type, GLuint id,
                    enum mesa_debug_severity severity,
                    GLsizei len, const char *buf)
{
   assert(!msg->message && !msg->length);

   msg->message = malloc(len+1);
   if (msg->message) {
      (void) strncpy(msg->message, buf, (size_t)len);
      msg->message[len] = '\0';

      msg->length = len+1;
      msg->source = source;
      msg->type = type;
      msg->id = id;
      msg->severity = severity;
   } else {
      static GLuint oom_msg_id = 0;
      debug_get_id(&oom_msg_id);

      /* malloc failed! */
      msg->message = out_of_memory;
      msg->length = strlen(out_of_memory)+1;
      msg->source = MESA_DEBUG_SOURCE_OTHER;
      msg->type = MESA_DEBUG_TYPE_ERROR;
      msg->id = oom_msg_id;
      msg->severity = MESA_DEBUG_SEVERITY_HIGH;
   }
}

static void
debug_namespace_init(struct gl_debug_namespace *ns)
{
   make_empty_list(&ns->Elements);

   /* Enable all the messages with severity HIGH or MEDIUM by default */
   ns->DefaultState = (1 << MESA_DEBUG_SEVERITY_HIGH) |
                      (1 << MESA_DEBUG_SEVERITY_MEDIUM);
}

static void
debug_namespace_clear(struct gl_debug_namespace *ns)
{
   struct simple_node *node, *tmp;

   foreach_s(node, tmp, &ns->Elements)
      free(node);
}

static bool
debug_namespace_copy(struct gl_debug_namespace *dst,
                     const struct gl_debug_namespace *src)
{
   struct simple_node *node;

   dst->DefaultState = src->DefaultState;

   make_empty_list(&dst->Elements);
   foreach(node, &src->Elements) {
      const struct gl_debug_element *elem =
         (const struct gl_debug_element *) node;
      struct gl_debug_element *copy;

      copy = malloc(sizeof(*copy));
      if (!copy) {
         debug_namespace_clear(dst);
         return false;
      }

      copy->ID = elem->ID;
      copy->State = elem->State;
      insert_at_tail(&dst->Elements, &copy->link);
   }

   return true;
}

/**
 * Set the state of \p id in the namespace.
 */
static bool
debug_namespace_set(struct gl_debug_namespace *ns,
                    GLuint id, bool enabled)
{
   const uint32_t state = (enabled) ?
      ((1 << MESA_DEBUG_SEVERITY_COUNT) - 1) : 0;
   struct gl_debug_element *elem = NULL;
   struct simple_node *node;

   /* find the element */
   foreach(node, &ns->Elements) {
      struct gl_debug_element *tmp = (struct gl_debug_element *) node;
      if (tmp->ID == id) {
         elem = tmp;
         break;
      }
   }

   /* we do not need the element if it has the default state */
   if (ns->DefaultState == state) {
      if (elem) {
         remove_from_list(&elem->link);
         free(elem);
      }
      return true;
   }

   if (!elem) {
      elem = malloc(sizeof(*elem));
      if (!elem)
         return false;

      elem->ID = id;
      insert_at_tail(&ns->Elements, &elem->link);
   }

   elem->State = state;

   return true;
}

/**
 * Set the default state of the namespace for \p severity.  When \p severity
 * is MESA_DEBUG_SEVERITY_COUNT, the default values for all severities are
 * updated.
 */
static void
debug_namespace_set_all(struct gl_debug_namespace *ns,
                        enum mesa_debug_severity severity,
                        bool enabled)
{
   struct simple_node *node, *tmp;
   uint32_t mask, val;

   /* set all elements to the same state */
   if (severity == MESA_DEBUG_SEVERITY_COUNT) {
      ns->DefaultState = (enabled) ? ((1 << severity) - 1) : 0;
      debug_namespace_clear(ns);
      make_empty_list(&ns->Elements);
      return;
   }

   mask = 1 << severity;
   val = (enabled) ? mask : 0;

   ns->DefaultState = (ns->DefaultState & ~mask) | val;

   foreach_s(node, tmp, &ns->Elements) {
      struct gl_debug_element *elem = (struct gl_debug_element *) node;

      elem->State = (elem->State & ~mask) | val;
      if (elem->State == ns->DefaultState) {
         remove_from_list(node);
         free(node);
      }
   }
}

/**
 * Get the state of \p id in the namespace.
 */
static bool
debug_namespace_get(const struct gl_debug_namespace *ns, GLuint id,
                    enum mesa_debug_severity severity)
{
   struct simple_node *node;
   uint32_t state;

   state = ns->DefaultState;
   foreach(node, &ns->Elements) {
      struct gl_debug_element *elem = (struct gl_debug_element *) node;

      if (elem->ID == id) {
         state = elem->State;
         break;
      }
   }

   return (state & (1 << severity));
}

/**
 * Allocate and initialize context debug state.
 */
static struct gl_debug_state *
debug_create(void)
{
   struct gl_debug_state *debug;
   int s, t;

   debug = CALLOC_STRUCT(gl_debug_state);
   if (!debug)
      return NULL;

   debug->Groups[0] = malloc(sizeof(*debug->Groups[0]));
   if (!debug->Groups[0]) {
      free(debug);
      return NULL;
   }

   /* Initialize state for filtering known debug messages. */
   for (s = 0; s < MESA_DEBUG_SOURCE_COUNT; s++) {
      for (t = 0; t < MESA_DEBUG_TYPE_COUNT; t++)
         debug_namespace_init(&debug->Groups[0]->Namespaces[s][t]);
   }

   return debug;
}

/**
 * Return true if the top debug group points to the group below it.
 */
static bool
debug_is_group_read_only(const struct gl_debug_state *debug)
{
   const GLint gstack = debug->GroupStackDepth;
   return (gstack > 0 && debug->Groups[gstack] == debug->Groups[gstack - 1]);
}

/**
 * Make the top debug group writable.
 */
static bool
debug_make_group_writable(struct gl_debug_state *debug)
{
   const GLint gstack = debug->GroupStackDepth;
   const struct gl_debug_group *src = debug->Groups[gstack];
   struct gl_debug_group *dst;
   int s, t;

   if (!debug_is_group_read_only(debug))
      return true;

   dst = malloc(sizeof(*dst));
   if (!dst)
      return false;

   for (s = 0; s < MESA_DEBUG_SOURCE_COUNT; s++) {
      for (t = 0; t < MESA_DEBUG_TYPE_COUNT; t++) {
         if (!debug_namespace_copy(&dst->Namespaces[s][t],
                                   &src->Namespaces[s][t])) {
            /* error path! */
            for (t = t - 1; t >= 0; t--)
               debug_namespace_clear(&dst->Namespaces[s][t]);
            for (s = s - 1; s >= 0; s--) {
               for (t = 0; t < MESA_DEBUG_TYPE_COUNT; t++)
                  debug_namespace_clear(&dst->Namespaces[s][t]);
            }
            free(dst);
            return false;
         }
      }
   }

   debug->Groups[gstack] = dst;

   return true;
}

/**
 * Free the top debug group.
 */
static void
debug_clear_group(struct gl_debug_state *debug)
{
   const GLint gstack = debug->GroupStackDepth;

   if (!debug_is_group_read_only(debug)) {
      struct gl_debug_group *grp = debug->Groups[gstack];
      int s, t;

      for (s = 0; s < MESA_DEBUG_SOURCE_COUNT; s++) {
         for (t = 0; t < MESA_DEBUG_TYPE_COUNT; t++)
            debug_namespace_clear(&grp->Namespaces[s][t]);
      }

      free(grp);
   }

   debug->Groups[gstack] = NULL;
}

/**
 * Loop through debug group stack tearing down states for
 * filtering debug messages.  Then free debug output state.
 */
static void
debug_destroy(struct gl_debug_state *debug)
{
   while (debug->GroupStackDepth > 0) {
      debug_clear_group(debug);
      debug->GroupStackDepth--;
   }

   debug_clear_group(debug);
   free(debug);
}

/**
 * Sets the state of the given message source/type/ID tuple.
 */
static void
debug_set_message_enable(struct gl_debug_state *debug,
                         enum mesa_debug_source source,
                         enum mesa_debug_type type,
                         GLuint id, GLboolean enabled)
{
   const GLint gstack = debug->GroupStackDepth;
   struct gl_debug_namespace *ns;

   debug_make_group_writable(debug);
   ns = &debug->Groups[gstack]->Namespaces[source][type];

   debug_namespace_set(ns, id, enabled);
}

/*
 * Set the state of all message IDs found in the given intersection of
 * 'source', 'type', and 'severity'.  The _COUNT enum can be used for
 * GL_DONT_CARE (include all messages in the class).
 *
 * This requires both setting the state of all previously seen message
 * IDs in the hash table, and setting the default state for all
 * applicable combinations of source/type/severity, so that all the
 * yet-unknown message IDs that may be used in the future will be
 * impacted as if they were already known.
 */
static void
debug_set_message_enable_all(struct gl_debug_state *debug,
                             enum mesa_debug_source source,
                             enum mesa_debug_type type,
                             enum mesa_debug_severity severity,
                             GLboolean enabled)
{
   const GLint gstack = debug->GroupStackDepth;
   int s, t, smax, tmax;

   if (source == MESA_DEBUG_SOURCE_COUNT) {
      source = 0;
      smax = MESA_DEBUG_SOURCE_COUNT;
   } else {
      smax = source+1;
   }

   if (type == MESA_DEBUG_TYPE_COUNT) {
      type = 0;
      tmax = MESA_DEBUG_TYPE_COUNT;
   } else {
      tmax = type+1;
   }

   debug_make_group_writable(debug);

   for (s = source; s < smax; s++) {
      for (t = type; t < tmax; t++) {
         struct gl_debug_namespace *nspace =
            &debug->Groups[gstack]->Namespaces[s][t];
         debug_namespace_set_all(nspace, severity, enabled);
      }
   }
}

/**
 * Returns if the given message source/type/ID tuple is enabled.
 */
static bool
debug_is_message_enabled(const struct gl_debug_state *debug,
                         enum mesa_debug_source source,
                         enum mesa_debug_type type,
                         GLuint id,
                         enum mesa_debug_severity severity)
{
   const GLint gstack = debug->GroupStackDepth;
   struct gl_debug_group *grp = debug->Groups[gstack];
   struct gl_debug_namespace *nspace = &grp->Namespaces[source][type];

   if (!debug->DebugOutput)
      return false;

   return debug_namespace_get(nspace, id, severity);
}

/**
 * 'buf' is not necessarily a null-terminated string. When logging, copy
 * 'len' characters from it, store them in a new, null-terminated string,
 * and remember the number of bytes used by that string, *including*
 * the null terminator this time.
 */
static void
debug_log_message(struct gl_debug_state *debug,
                  enum mesa_debug_source source,
                  enum mesa_debug_type type, GLuint id,
                  enum mesa_debug_severity severity,
                  GLsizei len, const char *buf)
{
   struct gl_debug_log *log = &debug->Log;
   GLint nextEmpty;
   struct gl_debug_message *emptySlot;

   assert(len >= 0 && len < MAX_DEBUG_MESSAGE_LENGTH);

   if (log->NumMessages == MAX_DEBUG_LOGGED_MESSAGES)
      return;

   nextEmpty = (log->NextMessage + log->NumMessages)
      % MAX_DEBUG_LOGGED_MESSAGES;
   emptySlot = &log->Messages[nextEmpty];

   debug_message_store(emptySlot, source, type,
                       id, severity, len, buf);

   log->NumMessages++;
}

/**
 * Return the oldest debug message out of the log.
 */
static const struct gl_debug_message *
debug_fetch_message(const struct gl_debug_state *debug)
{
   const struct gl_debug_log *log = &debug->Log;

   return (log->NumMessages) ? &log->Messages[log->NextMessage] : NULL;
}

/**
 * Delete the oldest debug messages out of the log.
 */
static void
debug_delete_messages(struct gl_debug_state *debug, unsigned count)
{
   struct gl_debug_log *log = &debug->Log;

   if (count > log->NumMessages)
      count = log->NumMessages;

   while (count--) {
      struct gl_debug_message *msg = &log->Messages[log->NextMessage];

      debug_message_clear(msg);

      log->NumMessages--;
      log->NextMessage++;
      log->NextMessage %= MAX_DEBUG_LOGGED_MESSAGES;
   }
}

static struct gl_debug_message *
debug_get_group_message(struct gl_debug_state *debug)
{
   return &debug->GroupMessages[debug->GroupStackDepth];
}

static void
debug_push_group(struct gl_debug_state *debug)
{
   const GLint gstack = debug->GroupStackDepth;

   /* just point to the previous stack */
   debug->Groups[gstack + 1] = debug->Groups[gstack];
   debug->GroupStackDepth++;
}

static void
debug_pop_group(struct gl_debug_state *debug)
{
   debug_clear_group(debug);
   debug->GroupStackDepth--;
}


/**
 * Return debug state for the context.  The debug state will be allocated
 * and initialized upon the first call.
 */
static struct gl_debug_state *
_mesa_get_debug_state(struct gl_context *ctx)
{
   if (!ctx->Debug) {
      ctx->Debug = debug_create();
      if (!ctx->Debug) {
         _mesa_error(ctx, GL_OUT_OF_MEMORY, "allocating debug state");
      }
   }

   return ctx->Debug;
}

/**
 * Set the integer debug state specified by \p pname.  This can be called from
 * _mesa_set_enable for example.
 */
bool
_mesa_set_debug_state_int(struct gl_context *ctx, GLenum pname, GLint val)
{
   struct gl_debug_state *debug = _mesa_get_debug_state(ctx);

   if (!debug)
      return false;

   switch (pname) {
   case GL_DEBUG_OUTPUT:
      debug->DebugOutput = (val != 0);
      break;
   case GL_DEBUG_OUTPUT_SYNCHRONOUS_ARB:
      debug->SyncOutput = (val != 0);
      break;
   default:
      assert(!"unknown debug output param");
      break;
   }

   return true;
}

/**
 * Query the integer debug state specified by \p pname.  This can be called
 * _mesa_GetIntegerv for example.
 */
GLint
_mesa_get_debug_state_int(struct gl_context *ctx, GLenum pname)
{
   struct gl_debug_state *debug;
   GLint val;

   debug = ctx->Debug;
   if (!debug)
      return 0;

   switch (pname) {
   case GL_DEBUG_OUTPUT:
      val = debug->DebugOutput;
      break;
   case GL_DEBUG_OUTPUT_SYNCHRONOUS_ARB:
      val = debug->SyncOutput;
      break;
   case GL_DEBUG_LOGGED_MESSAGES:
      val = debug->Log.NumMessages;
      break;
   case GL_DEBUG_NEXT_LOGGED_MESSAGE_LENGTH:
      val = (debug->Log.NumMessages) ?
         debug->Log.Messages[debug->Log.NextMessage].length : 0;
      break;
   case GL_DEBUG_GROUP_STACK_DEPTH:
      val = debug->GroupStackDepth;
      break;
   default:
      assert(!"unknown debug output param");
      val = 0;
      break;
   }

   return val;
}

/**
 * Query the pointer debug state specified by \p pname.  This can be called
 * _mesa_GetPointerv for example.
 */
void *
_mesa_get_debug_state_ptr(struct gl_context *ctx, GLenum pname)
{
   struct gl_debug_state *debug;
   void *val;

   debug = ctx->Debug;
   if (!debug)
      return NULL;

   switch (pname) {
   case GL_DEBUG_CALLBACK_FUNCTION_ARB:
      val = (void *) debug->Callback;
      break;
   case GL_DEBUG_CALLBACK_USER_PARAM_ARB:
      val = (void *) debug->CallbackData;
      break;
   default:
      assert(!"unknown debug output param");
      val = NULL;
      break;
   }

   return val;
}


/**
 * Log a client or driver debug message.
 */
static void
log_msg(struct gl_context *ctx, enum mesa_debug_source source,
        enum mesa_debug_type type, GLuint id,
        enum mesa_debug_severity severity, GLint len, const char *buf)
{
   struct gl_debug_state *debug = _mesa_get_debug_state(ctx);

   if (!debug)
      return;

   if (!debug_is_message_enabled(debug, source, type, id, severity))
      return;

   if (debug->Callback) {
       GLenum gl_type = debug_type_enums[type];
       GLenum gl_severity = debug_severity_enums[severity];

      debug->Callback(debug_source_enums[source], gl_type, id, gl_severity,
                      len, buf, debug->CallbackData);
      return;
   }

   debug_log_message(debug, source, type, id, severity, len, buf);
}


/**
 * Verify that source, type, and severity are valid enums.
 *
 * The 'caller' param is used for handling values available
 * only in glDebugMessageInsert or glDebugMessageControl
 */
static GLboolean
validate_params(struct gl_context *ctx, unsigned caller,
                const char *callerstr, GLenum source, GLenum type,
                GLenum severity)
{
#define INSERT 1
#define CONTROL 2
   switch(source) {
   case GL_DEBUG_SOURCE_APPLICATION_ARB:
   case GL_DEBUG_SOURCE_THIRD_PARTY_ARB:
      break;
   case GL_DEBUG_SOURCE_API_ARB:
   case GL_DEBUG_SOURCE_SHADER_COMPILER_ARB:
   case GL_DEBUG_SOURCE_WINDOW_SYSTEM_ARB:
   case GL_DEBUG_SOURCE_OTHER_ARB:
      if (caller != INSERT)
         break;
      else
         goto error;
   case GL_DONT_CARE:
      if (caller == CONTROL)
         break;
      else
         goto error;
   default:
      goto error;
   }

   switch(type) {
   case GL_DEBUG_TYPE_ERROR_ARB:
   case GL_DEBUG_TYPE_DEPRECATED_BEHAVIOR_ARB:
   case GL_DEBUG_TYPE_UNDEFINED_BEHAVIOR_ARB:
   case GL_DEBUG_TYPE_PERFORMANCE_ARB:
   case GL_DEBUG_TYPE_PORTABILITY_ARB:
   case GL_DEBUG_TYPE_OTHER_ARB:
   case GL_DEBUG_TYPE_MARKER:
      break;
   case GL_DEBUG_TYPE_PUSH_GROUP:
   case GL_DEBUG_TYPE_POP_GROUP:
   case GL_DONT_CARE:
      if (caller == CONTROL)
         break;
      else
         goto error;
   default:
      goto error;
   }

   switch(severity) {
   case GL_DEBUG_SEVERITY_HIGH_ARB:
   case GL_DEBUG_SEVERITY_MEDIUM_ARB:
   case GL_DEBUG_SEVERITY_LOW_ARB:
   case GL_DEBUG_SEVERITY_NOTIFICATION:
      break;
   case GL_DONT_CARE:
      if (caller == CONTROL)
         break;
      else
         goto error;
   default:
      goto error;
   }
   return GL_TRUE;

error:
   _mesa_error(ctx, GL_INVALID_ENUM, "bad values passed to %s"
               "(source=0x%x, type=0x%x, severity=0x%x)", callerstr,
               source, type, severity);

   return GL_FALSE;
}


static GLboolean
validate_length(struct gl_context *ctx, const char *callerstr, GLsizei length)
{
   if (length >= MAX_DEBUG_MESSAGE_LENGTH) {
      _mesa_error(ctx, GL_INVALID_VALUE,
                 "%s(length=%d, which is not less than "
                 "GL_MAX_DEBUG_MESSAGE_LENGTH=%d)", callerstr, length,
                 MAX_DEBUG_MESSAGE_LENGTH);
      return GL_FALSE;
   }

   return GL_TRUE;
}


void GLAPIENTRY
_mesa_DebugMessageInsert(GLenum source, GLenum type, GLuint id,
                         GLenum severity, GLint length,
                         const GLchar *buf)
{
   const char *callerstr = "glDebugMessageInsert";

   GET_CURRENT_CONTEXT(ctx);

   if (!validate_params(ctx, INSERT, callerstr, source, type, severity))
      return; /* GL_INVALID_ENUM */

   if (length < 0)
      length = strlen(buf);
   if (!validate_length(ctx, callerstr, length))
      return; /* GL_INVALID_VALUE */

   log_msg(ctx, gl_enum_to_debug_source(source),
           gl_enum_to_debug_type(type), id,
           gl_enum_to_debug_severity(severity),
           length, buf);
}


GLuint GLAPIENTRY
_mesa_GetDebugMessageLog(GLuint count, GLsizei logSize, GLenum *sources,
                         GLenum *types, GLenum *ids, GLenum *severities,
                         GLsizei *lengths, GLchar *messageLog)
{
   GET_CURRENT_CONTEXT(ctx);
   struct gl_debug_state *debug;
   GLuint ret;

   if (!messageLog)
      logSize = 0;

   if (logSize < 0) {
      _mesa_error(ctx, GL_INVALID_VALUE,
                  "glGetDebugMessageLog(logSize=%d : logSize must not be"
                  " negative)", logSize);
      return 0;
   }

   debug = _mesa_get_debug_state(ctx);
   if (!debug)
      return 0;

   for (ret = 0; ret < count; ret++) {
      const struct gl_debug_message *msg = debug_fetch_message(debug);

      if (!msg)
         break;

      if (logSize < msg->length && messageLog != NULL)
         break;

      if (messageLog) {
         assert(msg->message[msg->length-1] == '\0');
         (void) strncpy(messageLog, msg->message, (size_t)msg->length);

         messageLog += msg->length;
         logSize -= msg->length;
      }

      if (lengths)
         *lengths++ = msg->length;
      if (severities)
         *severities++ = debug_severity_enums[msg->severity];
      if (sources)
         *sources++ = debug_source_enums[msg->source];
      if (types)
         *types++ = debug_type_enums[msg->type];
      if (ids)
         *ids++ = msg->id;

      debug_delete_messages(debug, 1);
   }

   return ret;
}


void GLAPIENTRY
_mesa_DebugMessageControl(GLenum gl_source, GLenum gl_type,
                          GLenum gl_severity, GLsizei count,
                          const GLuint *ids, GLboolean enabled)
{
   GET_CURRENT_CONTEXT(ctx);
   enum mesa_debug_source source = gl_enum_to_debug_source(gl_source);
   enum mesa_debug_type type = gl_enum_to_debug_type(gl_type);
   enum mesa_debug_severity severity = gl_enum_to_debug_severity(gl_severity);
   const char *callerstr = "glDebugMessageControl";
   struct gl_debug_state *debug;

   if (count < 0) {
      _mesa_error(ctx, GL_INVALID_VALUE,
                  "%s(count=%d : count must not be negative)", callerstr,
                  count);
      return;
   }

   if (!validate_params(ctx, CONTROL, callerstr, gl_source, gl_type,
                        gl_severity))
      return; /* GL_INVALID_ENUM */

   if (count && (gl_severity != GL_DONT_CARE || gl_type == GL_DONT_CARE
                 || gl_source == GL_DONT_CARE)) {
      _mesa_error(ctx, GL_INVALID_OPERATION,
                  "%s(When passing an array of ids, severity must be"
         " GL_DONT_CARE, and source and type must not be GL_DONT_CARE.",
                  callerstr);
      return;
   }

   debug = _mesa_get_debug_state(ctx);
   if (!debug)
      return;

   if (count) {
      GLsizei i;
      for (i = 0; i < count; i++)
         debug_set_message_enable(debug, source, type, ids[i], enabled);
   }
   else {
      debug_set_message_enable_all(debug, source, type, severity, enabled);
   }
}


void GLAPIENTRY
_mesa_DebugMessageCallback(GLDEBUGPROC callback, const void *userParam)
{
   GET_CURRENT_CONTEXT(ctx);
   struct gl_debug_state *debug = _mesa_get_debug_state(ctx);
   if (debug) {
      debug->Callback = callback;
      debug->CallbackData = userParam;
   }
}


void GLAPIENTRY
_mesa_PushDebugGroup(GLenum source, GLuint id, GLsizei length,
                     const GLchar *message)
{
   GET_CURRENT_CONTEXT(ctx);
   struct gl_debug_state *debug = _mesa_get_debug_state(ctx);
   const char *callerstr = "glPushDebugGroup";
   struct gl_debug_message *emptySlot;

   if (!debug)
      return;

   if (debug->GroupStackDepth >= MAX_DEBUG_GROUP_STACK_DEPTH-1) {
      _mesa_error(ctx, GL_STACK_OVERFLOW, "%s", callerstr);
      return;
   }

   switch(source) {
   case GL_DEBUG_SOURCE_APPLICATION:
   case GL_DEBUG_SOURCE_THIRD_PARTY:
      break;
   default:
      _mesa_error(ctx, GL_INVALID_ENUM, "bad value passed to %s"
                  "(source=0x%x)", callerstr, source);
      return;
   }

   if (length < 0)
      length = strlen(message);
   if (!validate_length(ctx, callerstr, length))
      return; /* GL_INVALID_VALUE */

   log_msg(ctx, gl_enum_to_debug_source(source),
           MESA_DEBUG_TYPE_PUSH_GROUP, id,
           MESA_DEBUG_SEVERITY_NOTIFICATION, length,
           message);

   /* pop reuses the message details from push so we store this */
   emptySlot = debug_get_group_message(debug);
   debug_message_store(emptySlot,
                       gl_enum_to_debug_source(source),
                       gl_enum_to_debug_type(GL_DEBUG_TYPE_PUSH_GROUP),
                       id,
                       gl_enum_to_debug_severity(GL_DEBUG_SEVERITY_NOTIFICATION),
                       length, message);

   debug_push_group(debug);
}


void GLAPIENTRY
_mesa_PopDebugGroup(void)
{
   GET_CURRENT_CONTEXT(ctx);
   struct gl_debug_state *debug = _mesa_get_debug_state(ctx);
   const char *callerstr = "glPopDebugGroup";
   struct gl_debug_message *gdmessage;

   if (!debug)
      return;

   if (debug->GroupStackDepth <= 0) {
      _mesa_error(ctx, GL_STACK_UNDERFLOW, "%s", callerstr);
      return;
   }

   debug_pop_group(debug);

   gdmessage = debug_get_group_message(debug);
   log_msg(ctx, gdmessage->source,
           gl_enum_to_debug_type(GL_DEBUG_TYPE_POP_GROUP),
           gdmessage->id,
           gl_enum_to_debug_severity(GL_DEBUG_SEVERITY_NOTIFICATION),
           gdmessage->length, gdmessage->message);

   debug_message_clear(gdmessage);
}


void
_mesa_init_errors(struct gl_context *ctx)
{
   /* no-op */
}


void
_mesa_free_errors_data(struct gl_context *ctx)
{
   if (ctx->Debug) {
      debug_destroy(ctx->Debug);
      /* set to NULL just in case it is used before context is completely gone. */
      ctx->Debug = NULL;
   }
}


/**********************************************************************/
/** \name Diagnostics */
/*@{*/

static void
output_if_debug(const char *prefixString, const char *outputString,
                GLboolean newline)
{
   static int debug = -1;
   static FILE *fout = NULL;

   /* Init the local 'debug' var once.
    * Note: the _mesa_init_debug() function should have been called
    * by now so MESA_DEBUG_FLAGS will be initialized.
    */
   if (debug == -1) {
      /* If MESA_LOG_FILE env var is set, log Mesa errors, warnings,
       * etc to the named file.  Otherwise, output to stderr.
       */
      const char *logFile = _mesa_getenv("MESA_LOG_FILE");
      if (logFile)
         fout = fopen(logFile, "w");
      if (!fout)
         fout = stderr;
#ifdef DEBUG
      /* in debug builds, print messages unless MESA_DEBUG="silent" */
      if (MESA_DEBUG_FLAGS & DEBUG_SILENT)
         debug = 0;
      else
         debug = 1;
#else
      /* in release builds, be silent unless MESA_DEBUG is set */
      debug = _mesa_getenv("MESA_DEBUG") != NULL;
#endif
   }

   /* Now only print the string if we're required to do so. */
   if (debug) {
      fprintf(fout, "%s: %s", prefixString, outputString);
      if (newline)
         fprintf(fout, "\n");
      fflush(fout);

#if defined(_WIN32) && !defined(_WIN32_WCE)
      /* stderr from windows applications without console is not usually 
       * visible, so communicate with the debugger instead */ 
      {
         char buf[4096];
         _mesa_snprintf(buf, sizeof(buf), "%s: %s%s", prefixString, outputString, newline ? "\n" : "");
         OutputDebugStringA(buf);
      }
#endif
   }
}


/**
 * When a new type of error is recorded, print a message describing
 * previous errors which were accumulated.
 */
static void
flush_delayed_errors( struct gl_context *ctx )
{
   char s[MAX_DEBUG_MESSAGE_LENGTH];

   if (ctx->ErrorDebugCount) {
      _mesa_snprintf(s, MAX_DEBUG_MESSAGE_LENGTH, "%d similar %s errors", 
                     ctx->ErrorDebugCount,
                     _mesa_lookup_enum_by_nr(ctx->ErrorValue));

      output_if_debug("Mesa", s, GL_TRUE);

      ctx->ErrorDebugCount = 0;
   }
}


/**
 * Report a warning (a recoverable error condition) to stderr if
 * either DEBUG is defined or the MESA_DEBUG env var is set.
 *
 * \param ctx GL context.
 * \param fmtString printf()-like format string.
 */
void
_mesa_warning( struct gl_context *ctx, const char *fmtString, ... )
{
   char str[MAX_DEBUG_MESSAGE_LENGTH];
   va_list args;
   va_start( args, fmtString );  
   (void) _mesa_vsnprintf( str, MAX_DEBUG_MESSAGE_LENGTH, fmtString, args );
   va_end( args );
   
   if (ctx)
      flush_delayed_errors( ctx );

   output_if_debug("Mesa warning", str, GL_TRUE);
}


/**
 * Report an internal implementation problem.
 * Prints the message to stderr via fprintf().
 *
 * \param ctx GL context.
 * \param fmtString problem description string.
 */
void
_mesa_problem( const struct gl_context *ctx, const char *fmtString, ... )
{
   va_list args;
   char str[MAX_DEBUG_MESSAGE_LENGTH];
   static int numCalls = 0;

   (void) ctx;

   if (numCalls < 50) {
      numCalls++;

      va_start( args, fmtString );  
      _mesa_vsnprintf( str, MAX_DEBUG_MESSAGE_LENGTH, fmtString, args );
      va_end( args );
      fprintf(stderr, "Mesa %s implementation error: %s\n",
              PACKAGE_VERSION, str);
      fprintf(stderr, "Please report at " PACKAGE_BUGREPORT "\n");
   }
}


static GLboolean
should_output(struct gl_context *ctx, GLenum error, const char *fmtString)
{
   static GLint debug = -1;

   /* Check debug environment variable only once:
    */
   if (debug == -1) {
      const char *debugEnv = _mesa_getenv("MESA_DEBUG");

#ifdef DEBUG
      if (debugEnv && strstr(debugEnv, "silent"))
         debug = GL_FALSE;
      else
         debug = GL_TRUE;
#else
      if (debugEnv)
         debug = GL_TRUE;
      else
         debug = GL_FALSE;
#endif
   }

   if (debug) {
      if (ctx->ErrorValue != error ||
          ctx->ErrorDebugFmtString != fmtString) {
         flush_delayed_errors( ctx );
         ctx->ErrorDebugFmtString = fmtString;
         ctx->ErrorDebugCount = 0;
         return GL_TRUE;
      }
      ctx->ErrorDebugCount++;
   }
   return GL_FALSE;
}


void
_mesa_gl_debug(struct gl_context *ctx,
               GLuint *id,
               enum mesa_debug_type type,
               enum mesa_debug_severity severity,
               const char *fmtString, ...)
{
   char s[MAX_DEBUG_MESSAGE_LENGTH];
   int len;
   va_list args;

   debug_get_id(id);

   va_start(args, fmtString);
   len = _mesa_vsnprintf(s, MAX_DEBUG_MESSAGE_LENGTH, fmtString, args);
   va_end(args);

   log_msg(ctx, MESA_DEBUG_SOURCE_API, type, *id, severity, len, s);
}


/**
 * Record an OpenGL state error.  These usually occur when the user
 * passes invalid parameters to a GL function.
 *
 * If debugging is enabled (either at compile-time via the DEBUG macro, or
 * run-time via the MESA_DEBUG environment variable), report the error with
 * _mesa_debug().
 * 
 * \param ctx the GL context.
 * \param error the error value.
 * \param fmtString printf() style format string, followed by optional args
 */
void
_mesa_error( struct gl_context *ctx, GLenum error, const char *fmtString, ... )
{
   GLboolean do_output, do_log;
   /* Ideally this would be set up by the caller, so that we had proper IDs
    * per different message.
    */
   static GLuint error_msg_id = 0;

   debug_get_id(&error_msg_id);

   do_output = should_output(ctx, error, fmtString);
   if (ctx->Debug) {
      do_log = debug_is_message_enabled(ctx->Debug,
                                        MESA_DEBUG_SOURCE_API,
                                        MESA_DEBUG_TYPE_ERROR,
                                        error_msg_id,
                                        MESA_DEBUG_SEVERITY_HIGH);
   }
   else {
      do_log = GL_FALSE;
   }

   if (do_output || do_log) {
      char s[MAX_DEBUG_MESSAGE_LENGTH], s2[MAX_DEBUG_MESSAGE_LENGTH];
      int len;
      va_list args;

      va_start(args, fmtString);
      len = _mesa_vsnprintf(s, MAX_DEBUG_MESSAGE_LENGTH, fmtString, args);
      va_end(args);

      if (len >= MAX_DEBUG_MESSAGE_LENGTH) {
         /* Too long error message. Whoever calls _mesa_error should use
          * shorter strings.
          */
         ASSERT(0);
         return;
      }

      len = _mesa_snprintf(s2, MAX_DEBUG_MESSAGE_LENGTH, "%s in %s",
                           _mesa_lookup_enum_by_nr(error), s);
      if (len >= MAX_DEBUG_MESSAGE_LENGTH) {
         /* Same as above. */
         ASSERT(0);
         return;
      }

      /* Print the error to stderr if needed. */
      if (do_output) {
         output_if_debug("Mesa: User error", s2, GL_TRUE);
      }

      /* Log the error via ARB_debug_output if needed.*/
      if (do_log) {
         log_msg(ctx, MESA_DEBUG_SOURCE_API, MESA_DEBUG_TYPE_ERROR,
                 error_msg_id, MESA_DEBUG_SEVERITY_HIGH, len, s2);
      }
   }

   /* Set the GL context error state for glGetError. */
   _mesa_record_error(ctx, error);
}


/**
 * Report debug information.  Print error message to stderr via fprintf().
 * No-op if DEBUG mode not enabled.
 * 
 * \param ctx GL context.
 * \param fmtString printf()-style format string, followed by optional args.
 */
void
_mesa_debug( const struct gl_context *ctx, const char *fmtString, ... )
{
#ifdef DEBUG
   char s[MAX_DEBUG_MESSAGE_LENGTH];
   va_list args;
   va_start(args, fmtString);
   _mesa_vsnprintf(s, MAX_DEBUG_MESSAGE_LENGTH, fmtString, args);
   va_end(args);
   output_if_debug("Mesa", s, GL_FALSE);
#endif /* DEBUG */
   (void) ctx;
   (void) fmtString;
}


/**
 * Report debug information from the shader compiler via GL_ARB_debug_output.
 *
 * \param ctx GL context.
 * \param type The namespace to which this message belongs.
 * \param id The message ID within the given namespace.
 * \param msg The message to output. Need not be null-terminated.
 * \param len The length of 'msg'. If negative, 'msg' must be null-terminated.
 */
void
_mesa_shader_debug( struct gl_context *ctx, GLenum type, GLuint *id,
                    const char *msg, int len )
{
   enum mesa_debug_source source = MESA_DEBUG_SOURCE_SHADER_COMPILER;
   enum mesa_debug_severity severity = MESA_DEBUG_SEVERITY_HIGH;

   debug_get_id(id);

   if (len < 0)
      len = strlen(msg);

   /* Truncate the message if necessary. */
   if (len >= MAX_DEBUG_MESSAGE_LENGTH)
      len = MAX_DEBUG_MESSAGE_LENGTH - 1;

   log_msg(ctx, source, type, *id, severity, len, msg);
}

/*@}*/
