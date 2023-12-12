#include <string>
#include <iostream>
#include <sstream>
#include <map>
#include <vector>
#include <memory>

#define DO_LOG
#define DBG__XSTR(x) #x
#define DBG_XSTR(x) DBG__XSTR(x)
#ifdef DO_LOG
#define DBG(...)                                           \
do {                                                       \
    LOG_FN_ENTER();                                        \
    yed_log(__FILE__ ":" XSTR(__LINE__) ": " __VA_ARGS__); \
    LOG_EXIT();                                            \
} while (0)
#else
#define DBG(...) ;
#endif

using namespace std;

#include "json.hpp"
using json = nlohmann::json;

extern "C" {
    #include <yed/plugin.h>
}

static yed_plugin         *Self;

static string uri_for_buffer(yed_buffer *buffer) {
    string uri = "";

    if (!(buffer->flags & BUFF_SPECIAL)
    &&  buffer->kind == BUFF_KIND_FILE) {
        if (buffer->path == NULL) {
            uri += "untitled:";
            uri += buffer->name;
        } else {
            uri += "file://";
            uri += buffer->path;
        }
    }

    return uri;
}

struct Position {
    size_t line;
    size_t character;

    Position(size_t line, size_t character) : line(line), character(character) {}
};

static Position position_in_frame(yed_frame *frame) {
    yed_line *line;

    if (frame == NULL || frame->buffer == NULL) {
        return Position(-1, -1);
    }

    line = yed_buff_get_line(frame->buffer, frame->cursor_line);
    if (line == NULL) {
        return Position(-1, -1);
    }

    return Position(frame->cursor_line - 1, yed_line_col_to_idx(line, frame->cursor_col));
}

static void request(yed_frame *frame) {
    if (frame == NULL
    ||  frame->buffer == NULL
    ||  frame->buffer->kind != BUFF_KIND_FILE
    ||  frame->buffer->flags & BUFF_SPECIAL) {

        return;
    }

    string   uri = uri_for_buffer(frame->buffer);
    Position pos = position_in_frame(frame);

    json params = {
        { "textDocument", {
            { "uri", uri },
        }},
        { "position", {
            { "line",      pos.line      },
            { "character", pos.character },
        }},
    };


    yed_event event;
    string    text = params.dump();

    event.kind                       = EVENT_PLUGIN_MESSAGE;
    event.plugin_message.message_id  = "lsp-request:textDocument/definition";
    event.plugin_message.plugin_id   = "lsp_goto_definition";
    event.plugin_message.string_data = text.c_str();
    event.ft                         = frame->buffer->ft;

    yed_trigger_event(&event);
}

static void get_range(const json &result, yed_event *event) {
    int         row  = 0;
    int         byte = 0;
    yed_buffer *buffer;
    char        buff[512];
    string      b;

    if (result.contains("range")) {
        const auto &range = result["range"];
        row = range["start"]["line"];
        row += 1;
        byte = range["start"]["character"];
        int col;
        yed_line *line;
        line = yed_buff_get_line(ys->active_frame->buffer, row);
        col = yed_line_idx_to_col(line, byte);
        b = result["uri"].dump(2);
        b = b.substr(8, b.size() - 9);
        strcpy(buff, b.c_str());

        DBG("%s", buff);

        char a_path[512];
        char r_path[512];
        char h_path[512];
        char *name;
        abs_path(buff, a_path);
        relative_path_if_subtree(a_path, r_path);
        if (homeify_path(r_path, h_path)) {
            name = h_path;
        } else {
            name = r_path;
        }

        DBG("%s", name);
        buffer = yed_get_buffer(name);

        if (buffer == NULL) {
            buffer = yed_get_buffer_by_path(a_path);
            DBG("1");
        }

//         if (buffer == NULL) {
//             buffer = yed_create_buffer(name);
//             DBG("2");
//             if (buffer != NULL) {
//                 int status;
//                 status = yed_fill_buff_from_file(buffer, name);
//                 DBG("3");
//                 DBG("%s", name);
//                 DBG("status: %d", status);
//                 switch (status) {
//                     case BUFF_FILL_STATUS_ERR_DIR:
//                         yed_cerr("did not create buffer '%s' -- path is a directory", a_path);
//                         return;
//                     case BUFF_FILL_STATUS_ERR_PER:
//                         yed_cerr("did not create buffer '%s' -- permission denied", a_path);
//                         return;
//                     case BUFF_FILL_STATUS_ERR_MAP:
//                         yed_cerr("did not create buffer '%s' -- mmap() failed", a_path);
//                         return;
//                     case BUFF_FILL_STATUS_ERR_UNK:
//                         yed_cerr("did not create buffer '%s' -- unknown error", a_path);
//                         return;
//                     case BUFF_FILL_STATUS_ERR_NOF:
//                     case BUFF_FILL_STATUS_SUCCESS:
//                         yed_cprint("'%s' (new buffer)", name);
//                         if (status == BUFF_FILL_STATUS_ERR_NOF) {
//                             yed_cprint(" (new file)");
//                             buffer->path = strdup(a_path);
//                             buffer->kind = BUFF_KIND_FILE;
//                             buffer->ft   = FT_UNKNOWN;
//                             yed_buffer_set_ft(buffer, FT_UNKNOWN);
//                         }
//                 }

//             }
//         }
//         DBG("%s", a_path);
//         DBG("%s", buffer->path);
//         DBG("%d", buffer->kind);

        DBG("row:%d col:%d", row, col);

//         yed_frame_set_buff(ys->active_frame, buffer);

//         YEXE("buffer", buff);

        if (buffer && ys->active_frame) {
            yed_frame_set_buff(ys->active_frame, buffer);
            yed_move_cursor_within_frame(ys->active_frame, row - ys->active_frame->cursor_line, col - ys->active_frame->cursor_col);
        }

    } else if (result.contains("targetRange")) {
        yed_log("targetRange\n");

    }
}

static void pmsg(yed_event *event) {
    if (strcmp(event->plugin_message.plugin_id, "lsp") != 0
    ||  strcmp(event->plugin_message.message_id, "textDocument/definition") != 0) {
        return;
    }

    if (ys->active_frame == NULL) {
        return;
    }

    try {
        auto j = json::parse(event->plugin_message.string_data);
        yed_log("%s\n", j.dump(2).c_str());
        const auto &r = j["result"];

        if (r.is_array()) {
            const json &result = j["result"][0];
            get_range(result, event);

        } else {
            const json &result = j["result"];
            get_range(result, event);
        }

    } catch (...) {}

    event->cancel = 1;
}

static void unload(yed_plugin *self) { }

static void lsp_goto_implementation(int n_args, char **args) {
    if (ys->active_frame         == NULL
    ||  ys->active_frame->buffer == NULL) {

        return;
    }

    request(ys->active_frame);
}

extern "C"
int yed_plugin_boot(yed_plugin *self) {
    YED_PLUG_VERSION_CHECK();

    Self = self;

    map<void(*)(yed_event*), vector<yed_event_kind_t> > event_handlers = {
        { pmsg,     { EVENT_PLUGIN_MESSAGE    } },
    };

    map<const char*, const char*> vars          = { { "lsp-info-popup-idle-threshold-ms", XSTR(DEFAULT_THRESHOLD) } };
    map<const char*, void(*)(int, char**)> cmds = { { "lsp-goto-implementation", lsp_goto_implementation} };

    for (auto &pair : event_handlers) {
        for (auto evt : pair.second) {
            yed_event_handler h;
            h.kind = evt;
            h.fn   = pair.first;
            yed_plugin_add_event_handler(self, h);
        }
    }

    for (auto &pair : vars) {
        if (!yed_get_var(pair.first)) { yed_set_var(pair.first, pair.second); }
    }

    for (auto &pair : cmds) {
        yed_plugin_set_command(self, pair.first, pair.second);
    }

    /* Fake cursor move so that it works on startup/reload. */
    yed_move_cursor_within_active_frame(0, 0);

    yed_plugin_set_unload_fn(self, unload);

    return 0;
}
