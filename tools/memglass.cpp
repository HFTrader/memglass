// Generic interactive observer - works with any memglass session
// Tree-based browser with expandable/collapsible hierarchy
// Supports nested structs via field name prefixes (e.g., "quote.bid_price")
// Optional web server mode for browser-based viewing
#include <memglass/observer.hpp>

#include <fmt/format.h>
#include <termios.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <csignal>
#include <chrono>
#include <cstring>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>
#include <map>
#include <set>
#include <algorithm>

#ifdef MEMGLASS_WEB_ENABLED
#include <httplib.h>
#include <atomic>
#include <cmath>
#endif

static volatile bool g_running = true;

void signal_handler(int) {
    g_running = false;
}

// Format a field value based on its primitive type
std::string format_value(const memglass::FieldProxy& field) {
    auto* info = field.info();
    if (!info) return "<invalid>";

    switch (static_cast<memglass::PrimitiveType>(info->type_id)) {
        case memglass::PrimitiveType::Bool:
            return field.as<bool>() ? "true" : "false";
        case memglass::PrimitiveType::Int8:
            return std::to_string(static_cast<int>(field.as<int8_t>()));
        case memglass::PrimitiveType::UInt8:
            return std::to_string(static_cast<unsigned>(field.as<uint8_t>()));
        case memglass::PrimitiveType::Int16:
            return std::to_string(field.as<int16_t>());
        case memglass::PrimitiveType::UInt16:
            return std::to_string(field.as<uint16_t>());
        case memglass::PrimitiveType::Int32:
            return std::to_string(field.as<int32_t>());
        case memglass::PrimitiveType::UInt32:
            return std::to_string(field.as<uint32_t>());
        case memglass::PrimitiveType::Int64:
            return std::to_string(field.as<int64_t>());
        case memglass::PrimitiveType::UInt64:
            return std::to_string(field.as<uint64_t>());
        case memglass::PrimitiveType::Float32:
            return fmt::format("{:.6g}", field.as<float>());
        case memglass::PrimitiveType::Float64:
            return fmt::format("{:.6g}", field.as<double>());
        case memglass::PrimitiveType::Char:
            return fmt::format("'{}'", field.as<char>());
        default:
            return "<unknown>";
    }
}

std::string atomicity_str(memglass::Atomicity a) {
    switch (a) {
        case memglass::Atomicity::Atomic: return " [atomic]";
        case memglass::Atomicity::Seqlock: return " [seqlock]";
        case memglass::Atomicity::Locked: return " [locked]";
        default: return "";
    }
}

// Tree browser class
class TreeBrowser {
public:
    TreeBrowser(memglass::Observer& obs) : obs_(obs) {}

    void run() {
        // Set terminal to raw mode for single keypress detection
        struct termios old_term, new_term;
        tcgetattr(STDIN_FILENO, &old_term);
        new_term = old_term;
        new_term.c_lflag &= ~(ICANON | ECHO);
        new_term.c_cc[VMIN] = 0;
        new_term.c_cc[VTIME] = 0;
        tcsetattr(STDIN_FILENO, TCSANOW, &new_term);

        // Hide cursor
        std::cout << "\033[?25l";

        refresh_objects();
        render();

        while (g_running) {
            // Use select() to wait for input with 500ms timeout
            fd_set fds;
            FD_ZERO(&fds);
            FD_SET(STDIN_FILENO, &fds);

            struct timeval tv;
            tv.tv_sec = 0;
            tv.tv_usec = 500000;  // 500ms

            int ret = select(STDIN_FILENO + 1, &fds, nullptr, nullptr, &tv);

            if (ret > 0 && FD_ISSET(STDIN_FILENO, &fds)) {
                // Read available input using raw read()
                char buf[8];
                ssize_t n = read(STDIN_FILENO, buf, sizeof(buf));
                if (n > 0) {
                    char ch = buf[0];
                    if (ch == 'q' || ch == 'Q') {
                        break;
                    } else if (ch == '\033' && n >= 3 && buf[1] == '[') {
                        // Escape sequence for arrow keys
                        if (buf[2] == 'A') {  // Up arrow
                            move_up();
                        } else if (buf[2] == 'B') {  // Down arrow
                            move_down();
                        }
                    } else if (ch == 'k' || ch == 'K') {  // vim up
                        move_up();
                    } else if (ch == 'j' || ch == 'J') {  // vim down
                        move_down();
                    } else if (ch == '\n' || ch == '\r' || ch == ' ') {  // Enter or space
                        toggle_expand();
                    } else if (ch == 'r' || ch == 'R') {  // Refresh objects list
                        refresh_objects();
                    } else if (ch == 'h' || ch == 'H' || ch == '?') {  // Help
                        show_help_ = !show_help_;
                    }
                }
            }

            // Always render (auto-update values every 500ms)
            render();
        }

        // Show cursor
        std::cout << "\033[?25h";

        // Restore terminal
        tcsetattr(STDIN_FILENO, TCSANOW, &old_term);
    }

private:
    // Line types in the display
    enum class LineType { Object, FieldGroup, Field };

    struct DisplayLine {
        LineType type;
        size_t object_index;
        std::string field_group;     // For FieldGroup lines (e.g., "quote", "position")
        size_t field_index;          // For Field lines
        int indent;
        std::string display_name;
    };

    // Parsed field info for grouping
    struct FieldGroupInfo {
        std::string group_name;      // e.g., "quote" or "" for ungrouped
        std::string field_name;      // e.g., "bid_price"
        size_t original_index;       // Index in type's fields vector
    };

    void refresh_objects() {
        objects_ = obs_.objects();
    }

    // Parse field groups from a type (fields like "quote.bid_price" -> group "quote")
    std::map<std::string, std::vector<FieldGroupInfo>> get_field_groups(const memglass::ObservedType* type) {
        std::map<std::string, std::vector<FieldGroupInfo>> groups;

        if (!type) return groups;

        for (size_t i = 0; i < type->fields.size(); ++i) {
            const auto& field = type->fields[i];
            std::string full_name = field.name;

            FieldGroupInfo info;
            info.original_index = i;

            // Check for dot notation (nested struct)
            size_t dot_pos = full_name.find('.');
            if (dot_pos != std::string::npos) {
                info.group_name = full_name.substr(0, dot_pos);
                info.field_name = full_name.substr(dot_pos + 1);
            } else {
                info.group_name = "";  // Ungrouped
                info.field_name = full_name;
            }

            groups[info.group_name].push_back(info);
        }

        return groups;
    }

    void build_display_lines() {
        lines_.clear();

        for (size_t obj_idx = 0; obj_idx < objects_.size(); ++obj_idx) {
            const auto& obj = objects_[obj_idx];

            // Add object line
            DisplayLine obj_line;
            obj_line.type = LineType::Object;
            obj_line.object_index = obj_idx;
            obj_line.field_index = 0;
            obj_line.indent = 0;
            obj_line.display_name = obj.label;
            lines_.push_back(obj_line);

            // If object is expanded, add field groups and fields
            if (expanded_objects_.count(obj_idx)) {
                const memglass::ObservedType* type_info = nullptr;
                for (const auto& t : obs_.types()) {
                    if (t.name == obj.type_name) {
                        type_info = &t;
                        break;
                    }
                }

                if (type_info) {
                    auto field_groups = get_field_groups(type_info);

                    // Sort group names (empty string first for ungrouped fields)
                    std::vector<std::string> sorted_groups;
                    for (const auto& [name, _] : field_groups) {
                        sorted_groups.push_back(name);
                    }
                    std::sort(sorted_groups.begin(), sorted_groups.end());

                    for (const auto& group_name : sorted_groups) {
                        const auto& fields_in_group = field_groups[group_name];

                        if (group_name.empty()) {
                            // Ungrouped fields - add directly
                            for (const auto& fi : fields_in_group) {
                                DisplayLine field_line;
                                field_line.type = LineType::Field;
                                field_line.object_index = obj_idx;
                                field_line.field_group = "";
                                field_line.field_index = fi.original_index;
                                field_line.indent = 1;
                                field_line.display_name = fi.field_name;
                                lines_.push_back(field_line);
                            }
                        } else {
                            // Field group header
                            DisplayLine group_line;
                            group_line.type = LineType::FieldGroup;
                            group_line.object_index = obj_idx;
                            group_line.field_group = group_name;
                            group_line.field_index = 0;
                            group_line.indent = 1;
                            group_line.display_name = group_name;
                            lines_.push_back(group_line);

                            // If field group is expanded, add its fields
                            std::string expand_key = fmt::format("{}:{}", obj_idx, group_name);
                            if (expanded_field_groups_.count(expand_key)) {
                                for (const auto& fi : fields_in_group) {
                                    DisplayLine field_line;
                                    field_line.type = LineType::Field;
                                    field_line.object_index = obj_idx;
                                    field_line.field_group = group_name;
                                    field_line.field_index = fi.original_index;
                                    field_line.indent = 2;
                                    field_line.display_name = fi.field_name;
                                    lines_.push_back(field_line);
                                }
                            }
                        }
                    }
                }
            }
        }

        // Clamp cursor
        if (!lines_.empty() && cursor_ >= lines_.size()) {
            cursor_ = lines_.size() - 1;
        }
    }

    void render() {
        build_display_lines();

        // Get terminal size
        struct winsize ws;
        ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws);
        int term_height = ws.ws_row;
        int term_width = ws.ws_col;

        // Calculate visible range
        int header_lines = 3;
        int footer_lines = show_help_ ? 6 : 2;
        int visible_lines = term_height - header_lines - footer_lines;
        if (visible_lines < 1) visible_lines = 1;

        // Scroll to keep cursor visible
        if (cursor_ < scroll_offset_) {
            scroll_offset_ = cursor_;
        } else if (cursor_ >= scroll_offset_ + visible_lines) {
            scroll_offset_ = cursor_ - visible_lines + 1;
        }

        // Clear screen and move to top
        std::cout << "\033[2J\033[H";

        // Header
        auto now = std::chrono::steady_clock::now().time_since_epoch();
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now).count() % 100000;
        std::cout << "\033[1;36m=== Memglass Browser ===\033[0m\n";
        std::cout << "PID: " << obs_.producer_pid() << "  Objects: " << objects_.size();
        std::cout << "  Seq: " << obs_.sequence() << "  t:" << ms << "\n";
        std::cout << std::string(std::min(term_width, 80), '-') << "\n";

        // Content
        for (int i = 0; i < visible_lines && (scroll_offset_ + i) < lines_.size(); ++i) {
            size_t line_idx = scroll_offset_ + i;
            const auto& line = lines_[line_idx];
            bool is_selected = (line_idx == cursor_);

            // Selection highlight
            if (is_selected) {
                std::cout << "\033[7m";  // Reverse video
            }

            // Indent
            std::cout << std::string(line.indent * 2, ' ');

            if (line.type == LineType::Object) {
                const auto& obj = objects_[line.object_index];
                bool is_expanded = expanded_objects_.count(line.object_index);

                std::cout << (is_expanded ? "[-] " : "[+] ");
                std::cout << fmt::format("\033[1;33m{}\033[0m", obj.label);
                if (is_selected) std::cout << "\033[7m";
                std::cout << fmt::format(" \033[0;36m({})\033[0m", obj.type_name);
                if (is_selected) std::cout << "\033[7m";

            } else if (line.type == LineType::FieldGroup) {
                std::string expand_key = fmt::format("{}:{}", line.object_index, line.field_group);
                bool is_expanded = expanded_field_groups_.count(expand_key);

                std::cout << (is_expanded ? "[-] " : "[+] ");
                std::cout << fmt::format("\033[0;32m{}\033[0m", line.display_name);
                if (is_selected) std::cout << "\033[7m";

            } else {  // Field
                const auto& obj = objects_[line.object_index];
                const memglass::ObservedType* type_info = nullptr;
                for (const auto& t : obs_.types()) {
                    if (t.name == obj.type_name) {
                        type_info = &t;
                        break;
                    }
                }

                if (type_info && line.field_index < type_info->fields.size()) {
                    const auto& field = type_info->fields[line.field_index];

                    // Get field value
                    auto view = obs_.get(obj);
                    std::string value = "<unavailable>";
                    if (view) {
                        auto fv = view[field.name];
                        if (fv) {
                            value = format_value(fv);
                        }
                    }

                    std::cout << fmt::format("    \033[0;37m{:<16}\033[0m", line.display_name);
                    if (is_selected) std::cout << "\033[7m";
                    std::cout << " = ";
                    std::cout << fmt::format("\033[1;37m{:>14}\033[0m", value);
                    if (is_selected) std::cout << "\033[7m";

                    // Atomicity indicator
                    std::string atom = atomicity_str(field.atomicity);
                    if (!atom.empty()) {
                        std::cout << fmt::format("\033[0;35m{}\033[0m", atom);
                        if (is_selected) std::cout << "\033[7m";
                    }
                }
            }

            // Clear to end of line and reset
            std::cout << "\033[K\033[0m\n";
        }

        // Fill remaining lines
        for (size_t i = lines_.size() > scroll_offset_ ? lines_.size() - scroll_offset_ : 0;
             i < static_cast<size_t>(visible_lines); ++i) {
            std::cout << "\033[K\n";
        }

        // Footer
        std::cout << std::string(std::min(term_width, 80), '-') << "\n";

        if (show_help_) {
            std::cout << "\033[0;33mNavigation:\033[0m Up/Down or j/k  "
                      << "\033[0;33mExpand/Collapse:\033[0m Enter/Space\n";
            std::cout << "\033[0;33mRefresh:\033[0m r  "
                      << "\033[0;33mHelp:\033[0m h/?  "
                      << "\033[0;33mQuit:\033[0m q\n";
            std::cout << "\n";
            std::cout << "[+] = collapsed, [-] = expanded\n";
        } else {
            std::cout << "h/? for help | q to quit\n";
        }

        std::cout.flush();
    }

    void move_up() {
        if (cursor_ > 0) {
            cursor_--;
        }
    }

    void move_down() {
        build_display_lines();
        if (cursor_ + 1 < lines_.size()) {
            cursor_++;
        }
    }

    void toggle_expand() {
        build_display_lines();
        if (cursor_ >= lines_.size()) return;

        const auto& line = lines_[cursor_];

        if (line.type == LineType::Object) {
            // Toggle object expansion
            if (expanded_objects_.count(line.object_index)) {
                expanded_objects_.erase(line.object_index);
            } else {
                expanded_objects_.insert(line.object_index);
            }
        } else if (line.type == LineType::FieldGroup) {
            // Toggle field group expansion
            std::string expand_key = fmt::format("{}:{}", line.object_index, line.field_group);
            if (expanded_field_groups_.count(expand_key)) {
                expanded_field_groups_.erase(expand_key);
            } else {
                expanded_field_groups_.insert(expand_key);
            }
        }
        // Fields don't expand further
    }

    memglass::Observer& obs_;
    std::vector<memglass::ObservedObject> objects_;

    // Expansion state
    std::set<size_t> expanded_objects_;
    std::set<std::string> expanded_field_groups_;  // "obj_idx:group_name"

    // Display
    std::vector<DisplayLine> lines_;
    size_t cursor_ = 0;
    size_t scroll_offset_ = 0;
    bool show_help_ = false;
};

// ============================================================================
// Web Server Mode
// ============================================================================

#ifdef MEMGLASS_WEB_ENABLED

// JSON escaping helper
std::string json_escape(const std::string& s) {
    std::string result;
    result.reserve(s.size() + 10);
    for (char c : s) {
        switch (c) {
            case '"': result += "\\\""; break;
            case '\\': result += "\\\\"; break;
            case '\n': result += "\\n"; break;
            case '\r': result += "\\r"; break;
            case '\t': result += "\\t"; break;
            default: result += c; break;
        }
    }
    return result;
}

// Format field value as JSON-compatible string
std::string format_value_json(const memglass::FieldProxy& field) {
    auto* info = field.info();
    if (!info) return "null";

    switch (static_cast<memglass::PrimitiveType>(info->type_id)) {
        case memglass::PrimitiveType::Bool:
            return field.as<bool>() ? "true" : "false";
        case memglass::PrimitiveType::Int8:
            return std::to_string(static_cast<int>(field.as<int8_t>()));
        case memglass::PrimitiveType::UInt8:
            return std::to_string(static_cast<unsigned>(field.as<uint8_t>()));
        case memglass::PrimitiveType::Int16:
            return std::to_string(field.as<int16_t>());
        case memglass::PrimitiveType::UInt16:
            return std::to_string(field.as<uint16_t>());
        case memglass::PrimitiveType::Int32:
            return std::to_string(field.as<int32_t>());
        case memglass::PrimitiveType::UInt32:
            return std::to_string(field.as<uint32_t>());
        case memglass::PrimitiveType::Int64:
            return std::to_string(field.as<int64_t>());
        case memglass::PrimitiveType::UInt64:
            return std::to_string(field.as<uint64_t>());
        case memglass::PrimitiveType::Float32: {
            float v = field.as<float>();
            if (std::isnan(v)) return "\"NaN\"";
            if (std::isinf(v)) return v > 0 ? "\"Infinity\"" : "\"-Infinity\"";
            return fmt::format("{:.6g}", v);
        }
        case memglass::PrimitiveType::Float64: {
            double v = field.as<double>();
            if (std::isnan(v)) return "\"NaN\"";
            if (std::isinf(v)) return v > 0 ? "\"Infinity\"" : "\"-Infinity\"";
            return fmt::format("{:.6g}", v);
        }
        case memglass::PrimitiveType::Char:
            return fmt::format("\"{}\"", field.as<char>());
        default:
            return "null";
    }
}

std::string atomicity_json(memglass::Atomicity a) {
    switch (a) {
        case memglass::Atomicity::Atomic: return "\"atomic\"";
        case memglass::Atomicity::Seqlock: return "\"seqlock\"";
        case memglass::Atomicity::Locked: return "\"locked\"";
        default: return "\"none\"";
    }
}

// Embedded HTML/JS for the web UI
const char* WEB_UI_HTML = R"html(<!DOCTYPE html>
<html lang="en">
<head>
    <meta charset="UTF-8">
    <meta name="viewport" content="width=device-width, initial-scale=1.0">
    <title>Memglass Browser</title>
    <style>
        * { box-sizing: border-box; margin: 0; padding: 0; }
        body {
            font-family: 'SF Mono', 'Monaco', 'Inconsolata', 'Fira Code', monospace;
            background: #1a1a2e;
            color: #eee;
            padding: 20px;
            min-height: 100vh;
        }
        .header {
            background: linear-gradient(135deg, #16213e 0%, #1a1a2e 100%);
            padding: 20px;
            border-radius: 12px;
            margin-bottom: 20px;
            border: 1px solid #0f3460;
        }
        .header h1 {
            color: #00d9ff;
            font-size: 24px;
            margin-bottom: 10px;
        }
        .header .info {
            color: #888;
            font-size: 14px;
        }
        .header .info span {
            color: #00d9ff;
            margin-right: 20px;
        }
        .controls {
            margin: 15px 0;
            display: flex;
            gap: 10px;
            align-items: center;
        }
        .controls button {
            background: #0f3460;
            color: #00d9ff;
            border: 1px solid #00d9ff;
            padding: 8px 16px;
            border-radius: 6px;
            cursor: pointer;
            font-family: inherit;
            transition: all 0.2s;
        }
        .controls button:hover {
            background: #00d9ff;
            color: #1a1a2e;
        }
        .controls label {
            color: #888;
            font-size: 14px;
        }
        .controls input[type="checkbox"] {
            margin-right: 5px;
        }
        .tree {
            background: #16213e;
            border-radius: 12px;
            padding: 15px;
            border: 1px solid #0f3460;
        }
        .object {
            margin-bottom: 5px;
        }
        .object-header {
            display: flex;
            align-items: center;
            padding: 8px 12px;
            background: #0f3460;
            border-radius: 8px;
            cursor: pointer;
            transition: background 0.2s;
        }
        .object-header:hover {
            background: #1a4a7a;
        }
        .toggle {
            width: 20px;
            height: 20px;
            display: flex;
            align-items: center;
            justify-content: center;
            margin-right: 10px;
            color: #00d9ff;
            font-weight: bold;
        }
        .object-label {
            color: #ffd700;
            font-weight: bold;
            margin-right: 10px;
        }
        .object-type {
            color: #00d9ff;
            font-size: 13px;
        }
        .fields {
            margin-left: 30px;
            padding-left: 15px;
            border-left: 2px solid #0f3460;
        }
        .field-group-header {
            display: flex;
            align-items: center;
            padding: 6px 10px;
            margin: 3px 0;
            background: #1a2a4a;
            border-radius: 6px;
            cursor: pointer;
        }
        .field-group-header:hover {
            background: #1a3a5a;
        }
        .field-group-name {
            color: #4ade80;
            font-weight: bold;
        }
        .field {
            display: flex;
            align-items: center;
            padding: 5px 10px;
            margin: 2px 0;
            border-radius: 4px;
        }
        .field:hover {
            background: rgba(255,255,255,0.05);
        }
        .field-name {
            color: #aaa;
            width: 180px;
            flex-shrink: 0;
        }
        .field-value {
            color: #fff;
            font-weight: bold;
            min-width: 120px;
            text-align: right;
            margin-right: 10px;
        }
        .field-value.changed {
            animation: flash 0.3s ease-out;
        }
        @keyframes flash {
            0% { background: #ffd700; color: #000; }
            100% { background: transparent; color: #fff; }
        }
        .atomicity {
            font-size: 11px;
            padding: 2px 6px;
            border-radius: 4px;
            margin-left: 5px;
        }
        .atomicity.atomic { background: #7c3aed; color: #fff; }
        .atomicity.seqlock { background: #0891b2; color: #fff; }
        .atomicity.locked { background: #dc2626; color: #fff; }
        .status-bar {
            position: fixed;
            bottom: 0;
            left: 0;
            right: 0;
            background: #16213e;
            padding: 10px 20px;
            border-top: 1px solid #0f3460;
            display: flex;
            justify-content: space-between;
            font-size: 12px;
            color: #888;
        }
        .status-bar .live {
            color: #4ade80;
        }
        .hidden { display: none; }
    </style>
</head>
<body>
    <div class="header">
        <h1>Memglass Browser</h1>
        <div class="info">
            <span>PID: <b id="pid">-</b></span>
            <span>Objects: <b id="obj-count">-</b></span>
            <span>Sequence: <b id="sequence">-</b></span>
        </div>
        <div class="controls">
            <button onclick="refresh()">Refresh</button>
            <button onclick="expandAll()">Expand All</button>
            <button onclick="collapseAll()">Collapse All</button>
            <label>
                <input type="checkbox" id="auto-refresh" checked onchange="toggleAutoRefresh()">
                Auto-refresh (500ms)
            </label>
        </div>
    </div>
    <div class="tree" id="tree"></div>
    <div class="status-bar">
        <span>Last update: <span id="last-update">-</span></span>
        <span id="status" class="live">● Live</span>
    </div>

    <script>
        let data = { objects: [], types: [], pid: 0, sequence: 0 };
        let expanded = new Set();
        let expandedGroups = new Set();
        let previousValues = {};
        let autoRefreshEnabled = true;
        let refreshInterval = null;

        async function fetchData() {
            try {
                const resp = await fetch('/api/data');
                data = await resp.json();
                document.getElementById('pid').textContent = data.pid;
                document.getElementById('obj-count').textContent = data.objects.length;
                document.getElementById('sequence').textContent = data.sequence;
                document.getElementById('status').className = 'live';
                document.getElementById('status').textContent = '● Live';
            } catch (e) {
                document.getElementById('status').className = '';
                document.getElementById('status').textContent = '● Disconnected';
            }
        }

        function getFieldGroups(fields) {
            const groups = {};
            for (const field of fields) {
                const dotIdx = field.name.indexOf('.');
                if (dotIdx !== -1) {
                    const groupName = field.name.substring(0, dotIdx);
                    const fieldName = field.name.substring(dotIdx + 1);
                    if (!groups[groupName]) groups[groupName] = [];
                    groups[groupName].push({ ...field, displayName: fieldName });
                } else {
                    if (!groups['']) groups[''] = [];
                    groups[''].push({ ...field, displayName: field.name });
                }
            }
            return groups;
        }

        function render() {
            const tree = document.getElementById('tree');
            let html = '';

            for (let i = 0; i < data.objects.length; i++) {
                const obj = data.objects[i];
                const isExpanded = expanded.has(i);
                const type = data.types.find(t => t.name === obj.type_name);

                html += `<div class="object">`;
                html += `<div class="object-header" onclick="toggle(${i})">`;
                html += `<span class="toggle">${isExpanded ? '−' : '+'}</span>`;
                html += `<span class="object-label">${escapeHtml(obj.label)}</span>`;
                html += `<span class="object-type">(${escapeHtml(obj.type_name)})</span>`;
                html += `</div>`;

                if (isExpanded && type) {
                    html += `<div class="fields">`;
                    const groups = getFieldGroups(obj.fields || []);
                    const sortedGroupNames = Object.keys(groups).sort();

                    for (const groupName of sortedGroupNames) {
                        const fields = groups[groupName];
                        if (groupName === '') {
                            for (const field of fields) {
                                html += renderField(obj.label, field);
                            }
                        } else {
                            const groupKey = `${i}:${groupName}`;
                            const isGroupExpanded = expandedGroups.has(groupKey);
                            html += `<div class="field-group-header" onclick="toggleGroup('${groupKey}')">`;
                            html += `<span class="toggle">${isGroupExpanded ? '−' : '+'}</span>`;
                            html += `<span class="field-group-name">${escapeHtml(groupName)}</span>`;
                            html += `</div>`;
                            if (isGroupExpanded) {
                                html += `<div class="fields">`;
                                for (const field of fields) {
                                    html += renderField(obj.label, field);
                                }
                                html += `</div>`;
                            }
                        }
                    }
                    html += `</div>`;
                }
                html += `</div>`;
            }

            tree.innerHTML = html;
            document.getElementById('last-update').textContent = new Date().toLocaleTimeString();
        }

        function renderField(objLabel, field) {
            const key = `${objLabel}.${field.name}`;
            const prevValue = previousValues[key];
            const changed = prevValue !== undefined && prevValue !== field.value;
            previousValues[key] = field.value;

            let atomicityClass = '';
            let atomicityLabel = '';
            if (field.atomicity && field.atomicity !== 'none') {
                atomicityClass = field.atomicity;
                atomicityLabel = field.atomicity;
            }

            let html = `<div class="field">`;
            html += `<span class="field-name">${escapeHtml(field.displayName || field.name)}</span>`;
            html += `<span class="field-value${changed ? ' changed' : ''}">${formatValue(field.value)}</span>`;
            if (atomicityLabel) {
                html += `<span class="atomicity ${atomicityClass}">${atomicityLabel}</span>`;
            }
            html += `</div>`;
            return html;
        }

        function formatValue(v) {
            if (v === null || v === undefined) return '<null>';
            if (typeof v === 'number') {
                if (Number.isInteger(v)) return v.toLocaleString();
                return v.toLocaleString(undefined, { maximumFractionDigits: 6 });
            }
            return escapeHtml(String(v));
        }

        function escapeHtml(text) {
            const div = document.createElement('div');
            div.textContent = text;
            return div.innerHTML;
        }

        function toggle(idx) {
            if (expanded.has(idx)) expanded.delete(idx);
            else expanded.add(idx);
            render();
        }

        function toggleGroup(key) {
            if (expandedGroups.has(key)) expandedGroups.delete(key);
            else expandedGroups.add(key);
            render();
        }

        function expandAll() {
            for (let i = 0; i < data.objects.length; i++) {
                expanded.add(i);
                const type = data.types.find(t => t.name === data.objects[i].type_name);
                if (type) {
                    const groups = getFieldGroups(data.objects[i].fields || []);
                    for (const groupName of Object.keys(groups)) {
                        if (groupName) expandedGroups.add(`${i}:${groupName}`);
                    }
                }
            }
            render();
        }

        function collapseAll() {
            expanded.clear();
            expandedGroups.clear();
            render();
        }

        async function refresh() {
            await fetchData();
            render();
        }

        function toggleAutoRefresh() {
            autoRefreshEnabled = document.getElementById('auto-refresh').checked;
            if (autoRefreshEnabled) {
                startAutoRefresh();
            } else {
                stopAutoRefresh();
            }
        }

        function startAutoRefresh() {
            if (refreshInterval) return;
            refreshInterval = setInterval(refresh, 500);
        }

        function stopAutoRefresh() {
            if (refreshInterval) {
                clearInterval(refreshInterval);
                refreshInterval = null;
            }
        }

        // Initial load
        refresh().then(() => {
            // Auto-expand first object if there's only one
            if (data.objects.length === 1) {
                expanded.add(0);
                render();
            }
        });
        startAutoRefresh();
    </script>
</body>
</html>
)html";

class WebServer {
public:
    WebServer(memglass::Observer& obs, int port)
        : obs_(obs), port_(port), running_(false) {}

    void run() {
        httplib::Server svr;

        // Serve the main UI
        svr.Get("/", [](const httplib::Request&, httplib::Response& res) {
            res.set_content(WEB_UI_HTML, "text/html");
        });

        // API endpoint: get all data
        svr.Get("/api/data", [this](const httplib::Request&, httplib::Response& res) {
            obs_.refresh();
            std::string json = build_json();
            res.set_content(json, "application/json");
        });

        running_ = true;
        std::cerr << "Web server running at http://localhost:" << port_ << "\n";
        std::cerr << "Press Ctrl+C to stop.\n";

        // Run server in a way that can be interrupted
        svr.listen("0.0.0.0", port_);
    }

private:
    std::string build_json() {
        std::ostringstream ss;
        ss << "{";

        // Producer info
        ss << "\"pid\":" << obs_.producer_pid() << ",";
        ss << "\"sequence\":" << obs_.sequence() << ",";

        // Types
        ss << "\"types\":[";
        const auto& types = obs_.types();
        for (size_t i = 0; i < types.size(); ++i) {
            if (i > 0) ss << ",";
            const auto& t = types[i];
            ss << "{\"name\":\"" << json_escape(t.name) << "\""
               << ",\"type_id\":" << t.type_id
               << ",\"size\":" << t.size
               << ",\"field_count\":" << t.fields.size()
               << "}";
        }
        ss << "],";

        // Objects with field values
        ss << "\"objects\":[";
        auto objects = obs_.objects();
        for (size_t i = 0; i < objects.size(); ++i) {
            if (i > 0) ss << ",";
            const auto& obj = objects[i];
            ss << "{\"label\":\"" << json_escape(obj.label) << "\""
               << ",\"type_name\":\"" << json_escape(obj.type_name) << "\""
               << ",\"type_id\":" << obj.type_id
               << ",\"fields\":[";

            // Get field values
            auto view = obs_.get(obj);
            const memglass::ObservedType* type_info = nullptr;
            for (const auto& t : types) {
                if (t.name == obj.type_name) {
                    type_info = &t;
                    break;
                }
            }

            if (type_info && view) {
                for (size_t j = 0; j < type_info->fields.size(); ++j) {
                    if (j > 0) ss << ",";
                    const auto& field = type_info->fields[j];
                    auto fv = view[field.name];

                    ss << "{\"name\":\"" << json_escape(field.name) << "\""
                       << ",\"value\":" << (fv ? format_value_json(fv) : "null")
                       << ",\"atomicity\":" << atomicity_json(field.atomicity)
                       << "}";
                }
            }

            ss << "]}";
        }
        ss << "]";

        ss << "}";
        return ss.str();
    }

    memglass::Observer& obs_;
    int port_;
    std::atomic<bool> running_;
};

#endif // MEMGLASS_WEB_ENABLED

// ============================================================================
// Command-line parsing
// ============================================================================

struct Options {
    std::string session_name;
    bool web_mode = false;
    int web_port = 8080;
    bool help = false;
};

void print_usage(const char* prog) {
    std::cerr << "Usage: " << prog << " [OPTIONS] <session_name>\n"
              << "\n"
              << "Interactive observer for memglass sessions.\n"
              << "\n"
              << "Options:\n"
              << "  -h, --help           Show this help message\n"
#ifdef MEMGLASS_WEB_ENABLED
              << "  -w, --web [PORT]     Run as web server (default port: 8080)\n"
#endif
              << "\n"
              << "TUI Controls:\n"
              << "  Up/Down, j/k         Navigate\n"
              << "  Enter, Space         Expand/collapse\n"
              << "  r                    Refresh objects\n"
              << "  h, ?                 Toggle help\n"
              << "  q                    Quit\n";
}

Options parse_args(int argc, char* argv[]) {
    Options opts;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];

        if (arg == "-h" || arg == "--help") {
            opts.help = true;
            return opts;
        }
#ifdef MEMGLASS_WEB_ENABLED
        else if (arg == "-w" || arg == "--web") {
            opts.web_mode = true;
            // Check if next arg is a port number
            if (i + 1 < argc) {
                char* end;
                long port = std::strtol(argv[i + 1], &end, 10);
                if (*end == '\0' && port > 0 && port < 65536) {
                    opts.web_port = static_cast<int>(port);
                    ++i;
                }
            }
        }
#endif
        else if (arg[0] == '-') {
            std::cerr << "Unknown option: " << arg << "\n";
            opts.help = true;
            return opts;
        }
        else {
            opts.session_name = arg;
        }
    }

    return opts;
}

int main(int argc, char* argv[]) {
    std::signal(SIGINT, signal_handler);
    std::signal(SIGTERM, signal_handler);

    Options opts = parse_args(argc, argv);

    if (opts.help) {
        print_usage(argv[0]);
        return 1;
    }

    if (opts.session_name.empty()) {
        std::cerr << "Error: session name required\n\n";
        print_usage(argv[0]);
        return 1;
    }

    memglass::Observer obs(opts.session_name);

    std::cerr << "Connecting to session '" << opts.session_name << "'...\n";

    if (!obs.connect()) {
        std::cerr << "Failed to connect. Is the producer running?\n";
        return 1;
    }

    std::cerr << "Connected to PID: " << obs.producer_pid() << "\n";

#ifdef MEMGLASS_WEB_ENABLED
    if (opts.web_mode) {
        std::cerr << "Starting web server on port " << opts.web_port << "...\n";
        WebServer server(obs, opts.web_port);
        server.run();
    } else
#endif
    {
        std::cerr << "Starting TUI browser...\n";
        TreeBrowser browser(obs);
        browser.run();
    }

    std::cout << "\nDisconnecting...\n";
    obs.disconnect();

    return 0;
}
