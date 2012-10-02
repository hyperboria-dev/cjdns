/* vim: set expandtab ts=4 sw=4: */
/*
 * You may redistribute this program and/or modify it under the terms of
 * the GNU General Public License as published by the Free Software Foundation,
 * either version 3 of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */
#include "io/Writer.h"
#include "util/Log.h"

#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#define MAX_SUBSCRIPTIONS 64
#define FILENAME_COUNT 32

struct Subscription
{
    /** The log level to match against, all higher levels will also be matched. */
    enum Log_Level level;

    /**
     * true if the file name is the internal name
     * which can be compared using a pointer equality check
     */
    bool internalName : 1;

    /** The line number within the file or 0 to match all lines. */
    int lineNum : 31;

    /** The name of the file to match against or null to match any file. */
    char* file;

    /** The transaction ID of the message which solicited this stream of logs. */
    String* txid;

    /** A hopefully unique (random) number identifying this stream. */
    uint64_t streamId;

    /** An allocator which will live during the lifecycle of the Subscription */
    struct Allocator* alloc;
};

struct AdminLog
{
    struct Log pub;
    struct Subscription subscriptions[MAX_SUBSCRIPTIONS];
    uint32_t subscriptionCount;
    char* fileNames[FILENAME_COUNT];
    struct Admin* admin;
    struct Allocator* alloc;
};

static inline bool isMatch(struct Subscription* subscription,
                           struct AdminLog* logger,
                           enum Log_Level logLevel,
                           const char* file,
                           uint32_t line)
{
    if (subscription->file) {
        if (subscription->internalName) {
            if (file != subscription->file) {
                return false;
            }
        } else if (strcmp(file, subscription->file)) {
            return false;
        } else {
            // It's the same name but so we'll swap the name for the internal name and then
            // it can be compared quickly with a pointer comparison.
            subscription->file = file;
            subscription->internalName = true;
            for (int i = 0; i < FILENAME_COUNT; i++) {
                if (logger->fileNames[i] == file) {
                    break;
                }
                if (logger->fileNames[i] == NULL) {
                    logger->fileNames[i] = file;
                    logger->fileNames[(i + 1) % FILE_NAME_COUNT] = NULL;
                    break;
                }
            }
        }
    }

    if (logLevel < subscription->level) {
        return false;
    }
    if (subscription->line && line != subscription->line) {
        return false;
    }
    return true;
}

static Dict* makeLogMessage(struct AdminLog* logger,
                            enum Log_Level logLevel,
                            const char* file,
                            uint32_t line,
                            const char* format,
                            va_list* vaArgs,
                            uint32_t streamNum,
                            struct Allocator* alloc)
{
    time_t now;
    time(&now);

    Dict* out = Dict_new(alloc);
    Dict_putInt(out, String_CONST("streamId"), streamNum, alloc);
    Dict_putInt(out, String_CONST("time"), now, alloc);
    Dict_putString(out, String_CONST("level"), String_CONST(Log_nameForLevel(logLevel)), alloc);
    Dict_putString(out, String_CONST("file"), String_CONST(file), alloc);
    Dict_putInt(out, String_CONST("line"), line, alloc);
    String* message = String_vprintf(alloc, format, vaArgs);
    Dict_putString(out, String_CONST("message"), message, alloc);

    return out;
}

static void doLog(struct Log* genericLog,
                  enum Log_Level logLevel,
                  const char* file,
                  uint32_t line,
                  const char* format,
                  ...)
{
    struct AdminLog* log = (struct AdminLog*) genericLog;
    Dict* message = NULL;
    #define ALLOC_BUFFER_SZ 4096
    uint8_t allocBuffer[ALLOC_BUFFER_SZ];
    for (int i = 0; i < log->subscriptionCount; i++) {
        if (isMatch(&log->subscriptions[i], log, logLevel, file, line)) {
            if (!message) {
                struct Allocator* alloc = BufferAllocator_new(allocBuffer, ALLOC_BUFFER_SZ);
                va_list args;
                va_start(args, format);
                message = makeLogMessage(log, logLevel, file, line, format, args, alloc);
                va_end(args);
            }
            Admin_sendMessage(message, log->subscriptions[i].txid, log->admin);
        }
    }
}

static void subscribe(Dict* args, void* vcontext, String* txid)
{
    struct AdminLog* log = (struct AdminLog*) vcontext;
    String* levelName = Dict_getString(args, String_CONST("level"));
    struct Log_Level level = (levelName) ? Log_levelForName(levelName) : Log_Level_DEBUG;
    int64_t* lineNumPtr = Dict_getInt(args, String_CONST("line"));
    String* fileStr = Dict_getString(args, String_CONST("file"));
    char* file = (fileStr) ? fileStr->bytes : NULL;
    char* error = "2+2=5";
    if (level == Log_Level_INVALID) {
        error = "The provided log level is invalid, please specify one of ["
        #ifdef Log_KEYS
            "KEYS, "
        #endif
        #ifdef Log_DEBUG
            "DEBUG, "
        #endif
        #ifdef Log_INFO
            "INFO, "
        #endif
        #ifdef Log_WARN
            "WARN, "
        #endif
        #ifdef Log_ERROR
            "ERROR, "
        #endif
        "CRITICAL]";
    } else if (lineNumPtr && *lineNumPtr < 1) {
        error = "Invalid line number, must be greater than or equal to 1";
    } else if (log->subscriptionCount >= MAX_SUBSCRIPTIONS) {
        error = "Max subscription count reached.";
    } else {
        struct Subscription* sub = &log->subscriptions[log->subscriptionCount];
        sub->level = level;
        sub->alloc = log->alloc->child(log->alloc);
        if (file) {
            int i;
            for (i = 0; i < FILENAME_COUNT; i++) {
                if (log->fileNames[i] && !strcmp(log->fileNames[i], file)) {
                    file = fileNames[i];
                    sub->internalName = true;
                    break;
                }
            }
            if (i == FILENAME_COUNT) {
                file = String_new(file, sub->alloc)->bytes;
                sub->internalName = false;
            }
        }
        sub->file = file;
        sub->lineNum = (lineNumPtr) ? *lineNumPtr : 0;
        sub->txid = String_CLONE(txid, sub->alloc);
        randombytes(&sub->streamId, 8);
        uint8_t streamIdHex[20];
        Hex_encode(streamIdHex, 20, &sub->streamId, 8);
        Dict response = Dict_new(
            String_CONST("error"), String_OBJ(String_CONST("none")), Dict_new(
            String_CONST("streamId"), String_OBJ(String_CONST(streamIdHex)), NULL
        ));
        Admin_sendMessage(&response, txid, log->admin);
        log->subscriptionCount++;
        return;
    }

    Dict response = Dict_new(
        String_CONST("error"), String_OBJ(String_CONST(error)), NULL
    );
    Admin_sendMessage(&response, txid, log->admin);
}

static void unsubscribe(Dict* args, void* vcontext, String* txid)
{
    struct AdminLog* log = (struct AdminLog*) vcontext;
    String* streamIdHex = Dict_getString(args, String_CONST("streamId"));
    uint64_t streamId;
    char* error = NULL;
    if (streamIdHex->len != 16 || Hex_decode(&streamId, 8, streamIdHex->bytes, 16) != 8) {
        error = "Invalid streamId.";
    } else {
        error = "No such subscription."
        for (int i = 0; i < log->subscriptionCount; i++) {
            if (streamId == log->subscriptions[i].streamId) {
                log->subscriptions[i].alloc->free(log->subscriptions[i].alloc);
                log->subscriptionCount--;
                if (log->subscriptionCount) {
                    Bits_memcpyConst(&log->subscriptions[i],
                                     &log->subscriptions[log->subscriptionCount],
                                     sizeof(struct Subscription));
                }
                error = "none";
                break;
            }
        }
    }

    Dict response = Dict_new(
        String_CONST("error"), String_OBJ(String_CONST(error)), NULL
    );
    Admin_sendMessage(&response, txid, log->admin);
}

struct Log* AdminLog_registerNew(struct Admin* admin, struct Allocator* alloc)
{
    struct AdminLog* log = alloc->clone(sizeof(struct AdminLog), alloc, &(struct AdminLog) {
        .pub = {
            .callback = doLog
        },
        .admin = admin,
        .alloc = alloc
    });

    struct Admin_FunctionArg subscribeArgs[] = {
        { .name = "level", .required = 0, .type = "String" },
        { .name = "line", .required = 0, .type = "Int" },
        { .name = "file", .required = 0, .type = "String" }
    };
    Admin_registerFunction("AdminLog_subscribe", subscribe, log, true, subscribeArgs, admin);

    struct Admin_FunctionArg unsubscribeArgs[] = {
        { .name = "streamId", .required = 1, .type = "String" }
    };
    Admin_registerFunction("AdminLog_unsubscribe", unsubscribe, log, true, unsubscribeArgs, admin);
}
