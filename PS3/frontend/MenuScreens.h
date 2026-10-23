// Copyright (c) 2012- PPSSPP Project.

// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, version 2.0 or later versions.

// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License 2.0 for more details.

// A copy of the GPL 2.0 should have been included with the program.
// If not, see http://www.gnu.org/licenses/

// Official git repository and contact information can be found at
// https://github.com/hrydgard/ppsspp and http://www.ppsspp.org/.

#pragma once

#include <cstdint>
#include <string>
#include <vector>

// Layouts of the frontend's menu screens, drawn with Canvas (no SDL, so they can be previewed on a PC).
namespace MenuScreens {

struct SettingRow {
	std::string label;
	std::string value;
	bool disabled = false;  // Shown greyed out (it has no effect with the current settings).
	bool header = false;    // A section heading (label only), never selected.
};

// The bar of tabs along the top of the main screens.
struct Tabs {
	const char *const *names = nullptr;
	int count = 0;
	int active = 0;      // The page shown below the bar.
	int highlight = -1;  // The tab with the cursor, or -1 when the cursor is on the page.
};

// Two columns: rows [0, split) on the left, [split, count) on the right; help describes the selection
// (-1: no selection, the cursor is on the tabs), hint lists the buttons.
void Settings(double seconds, const Tabs &tabs, const char *version, const std::vector<SettingRow> &rows, int split,
	const char *leftTitle, const char *rightTitle, int selection, const std::string &help, const std::string &hint);

enum { GRID_COLUMNS = 5, GRID_ROWS = 3, ICON_W = 216, ICON_H = 120 };

struct GameTile {
	std::string title;
	const uint32_t *icon = nullptr;  // ICON_W x ICON_H pixels (0xAARRGGBB), or null.
	std::string kind;                // Shown instead of a missing icon ("ISO", "CSO"...).
	bool loading = false;            // The icon hasn't been read yet.
};

// Buttons along the top of the games page (button: the one with the cursor, or -1), status on their
// right, then GRID_ROWS rows of game icons from topRow (selection -1: the cursor isn't on a game).
void Games(double seconds, const Tabs &tabs, const char *version, const std::vector<std::string> &buttons, int button,
	const std::string &status, const std::vector<GameTile> &games, int selection, int topRow, const std::string &hint,
	const char *emptyFolder);

// Small window over the screen drawn last: title, wrapped text, then choices.
void Popup(const std::string &title, const std::string &text, const std::vector<std::string> &items, int selection, const std::string &hint);

// Scrolled list of report lines (PASS green, FAIL red); note is shown top right.
void Report(double seconds, const char *title, const std::vector<std::string> &lines, size_t top, size_t visible, const char *note = "saved in USRDIR");
size_t ReportVisibleLines();

// A text file to read: lines top.. (visible of them), the selected one highlighted, comment lines
// (# or ;) dimmed; then a status line and the hint.
void Viewer(double seconds, const char *title, const char *note, const std::vector<std::string> &lines, size_t top, size_t visible,
	int selected, const std::string &status, const std::string &hint);

// Title, wrapped message and up to two footer lines.
void Message(double seconds, const char *title, const std::string &message, const std::string &footer1, const std::string &footer2);

// Text editor with an on-screen keyboard: lines top.. of the text (cursor at curLine, curCol), the
// keyboard rows below (keys of a row share its width; keyRow, keyCol selected), then a status
// line and the hint.
enum { EDITOR_VISIBLE_LINES = 9, EDITOR_COLUMNS = 94 };
void Editor(double seconds, const char *title, const char *note, const std::vector<std::string> &lines, int top, int curLine, int curCol,
	const std::vector<std::vector<std::string>> &keys, int keyRow, int keyCol, const std::string &status, const std::string &hint);

// Only the background and one line of large text in the middle (while the app restarts).
void Splash(double seconds, const char *text);

}  // namespace MenuScreens
