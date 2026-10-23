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

#include "MenuScreens.h"
#include "Canvas.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace MenuScreens {

// Layout: the TV may crop up to ~5% at each edge, so everything stays inside this area.
enum { LEFT = 56, RIGHT = 1224, TOP = 30 };

static const uint32_t TITLE = 0xFFFFFF;
static const uint32_t LABEL = 0xA9C2E2;
static const uint32_t VALUE = 0xFFFFFF;
static const uint32_t DISABLED = 0x6A7A94;
static const uint32_t ACCENT = 0x7CCBFF;
static const uint32_t SELECTED = 0xFFE68C;
static const uint32_t HINT = 0xC8D6EA;
static const uint32_t DIM = 0x8FA3BF;
static const uint32_t SELECT_BAR = 0x4FA2FF;

// Splits text into lines of at most width characters at spaces.
static std::vector<std::string> Wrap(const std::string &text, size_t width) {
	std::vector<std::string> lines;
	size_t paragraph = 0;
	while (paragraph <= text.size()) {
		size_t newline = text.find('\n', paragraph);
		if (newline == std::string::npos) newline = text.size();
		const std::string part = text.substr(paragraph, newline - paragraph);
		size_t start = 0;
		do {
			size_t end = std::min(part.size(), start + width);
			if (end < part.size() && part[end] != ' ') {
				const size_t space = part.rfind(' ', end);
				if (space != std::string::npos && space > start)
					end = space;
			}
			lines.push_back(part.substr(start, end - start));
			start = end;
			while (start < part.size() && part[start] == ' ')
				++start;
		} while (start < part.size());
		paragraph = newline + 1;
	}
	return lines;
}

static std::string Clip(const std::string &text, size_t chars) {
	if (text.size() <= chars) return text;
	return text.substr(0, chars > 3 ? chars - 3 : 0) + "...";
}

static void Header(double seconds, const char *title, const char *right) {
	Canvas::Background(seconds);
	Canvas::Text(LEFT + 8, TOP + 4, title, TITLE, true);
	if (right && *right)
		Canvas::Text(RIGHT - 8 - Canvas::TextWidth(right), TOP + 22, right, DIM);
}

static void TabBar(double seconds, const Tabs &tabs, const char *right) {
	Canvas::Background(seconds);
	Canvas::Text(LEFT + 8, TOP, "PPSSPP", TITLE, true);
	int x = LEFT + 8 + Canvas::TextWidth("PPSSPP", true) + 32;
	const int y = TOP + 6, h = 44;
	for (int i = 0; i < tabs.count; ++i) {
		const int w = Canvas::TextWidth(tabs.names[i]) + 28;
		const bool lit = i == tabs.highlight, active = i == tabs.active;
		if (lit)
			Canvas::Bar(x, y, w, h, SELECT_BAR, 120);
		else if (active)
			Canvas::Bar(x, y, w, h, SELECT_BAR, 45);
		if (active)
			Canvas::Bar(x + 12, y + h - 6, w - 24, 3, ACCENT, 220);
		Canvas::Text(x + 14, y + 10, tabs.names[i], lit ? SELECTED : (active ? TITLE : LABEL));
		x += w + 4;
	}
	if (right && *right) {
		const int rx = RIGHT - 8 - Canvas::TextWidth(right);
		if (rx > x + 8)
			Canvas::Text(rx, y + 10, right, DIM);
	}
}

void Settings(double seconds, const Tabs &tabs, const char *version, const std::vector<SettingRow> &rows, int split,
	const char *leftTitle, const char *rightTitle, int selection, const std::string &help, const std::string &hint) {
	TabBar(seconds, tabs, version);
	const int panelY = 92, panelH = 456;
	// Without column titles (the rows have their own section headings) the rows start higher.
	const bool titled = (leftTitle && *leftTitle) || (rightTitle && *rightTitle);
	const int firstRow = titled ? 134 : 106;
	// 21 pixels a row (19 rows a column on screen); a longer column scrolls with the selection
	// (build 147: squeezing all rows in made the selection bar overlap the next row).
	const int rowH = 21;
	const int visible = (panelY + panelH - firstRow) / rowH;
	const int colW = (RIGHT - LEFT - 16) / 2;
	const int colX[2] = {LEFT, LEFT + colW + 16};
	const char *titles[2] = {leftTitle, rightTitle};
	const int colStart[2] = {0, split};
	const int colCount[2] = {split, (int)rows.size() - split};
	static int top[2] = {0, 0};
	for (int col = 0; col < 2; ++col) {
		const int selIndex = selection - colStart[col];
		if (selIndex >= 0 && selIndex < colCount[col]) {
			if (selIndex < top[col]) top[col] = selIndex;
			if (selIndex >= top[col] + visible) top[col] = selIndex - visible + 1;
		}
		top[col] = std::max(0, std::min(top[col], colCount[col] - visible));
	}
	for (int col = 0; col < 2; ++col) {
		Canvas::Panel(colX[col], panelY, colW, panelH, 125);
		Canvas::Text(colX[col] + 16, panelY + 10, titles[col], ACCENT);
		// More rows above or below.
		std::string more;
		if (top[col] > 0) more += "^";
		if (top[col] + visible < colCount[col]) more += more.empty() ? "v more" : " v more";
		else if (!more.empty()) more += " more";
		if (!more.empty())
			Canvas::Text(colX[col] + colW - 16 - Canvas::TextWidth(more.c_str()), panelY + 10, more.c_str(), DIM);
	}
	const int valueX = 16 + 21 * Canvas::CharWidth();
	const size_t valueChars = (size_t)(colW - valueX - 14) / Canvas::CharWidth();
	int gap[2] = {0, 0};  // A little space above each heading that isn't at the top of its column.
	for (int i = 0; i < (int)rows.size(); ++i) {
		const int col = i < split ? 0 : 1;
		const int index = i - colStart[col] - top[col];
		if (index < 0 || index >= visible)
			continue;
		if (rows[i].header && index > 0)
			gap[col] += 8;
		const int x = colX[col], y = firstRow + index * rowH + gap[col];
		const bool sel = i == selection;
		if (rows[i].header) {
			Canvas::Text(x + 16, y, rows[i].label.c_str(), ACCENT);
			continue;
		}
		if (sel)
			Canvas::Bar(x + 6, y - 1, colW - 12, rowH, SELECT_BAR, 80);
		const bool off = rows[i].disabled;
		Canvas::Text(x + 16, y, Clip(rows[i].label, 20).c_str(), sel ? SELECTED : (off ? DISABLED : LABEL));
		Canvas::Text(x + valueX, y, Clip(rows[i].value, valueChars).c_str(), sel ? SELECTED : (off ? DISABLED : VALUE));
	}
	// Help for the selected option.
	const int helpY = 556, helpH = 128;
	Canvas::Panel(LEFT, helpY, RIGHT - LEFT, helpH, 125);
	const size_t helpChars = (size_t)(RIGHT - LEFT - 32) / Canvas::CharWidth();
	std::vector<std::string> lines = Wrap(help, helpChars);
	for (size_t i = 0; i < lines.size() && i < 3; ++i)
		Canvas::Text(LEFT + 16, helpY + 11 + (int)i * 27, lines[i].c_str(), HINT);
	Canvas::Text(LEFT + 16, helpY + 98, Clip(hint, helpChars).c_str(), DIM);
}

void Games(double seconds, const Tabs &tabs, const char *version, const std::vector<std::string> &buttons, int button,
	const std::string &status, const std::vector<GameTile> &games, int selection, int topRow, const std::string &hint,
	const char *emptyFolder) {
	TabBar(seconds, tabs, version);
	const int panelY = 92, panelH = 530;
	Canvas::Panel(LEFT, panelY, RIGHT - LEFT, panelH, 110);
	// Buttons.
	int bx = LEFT + 12;
	const int by = panelY + 8, bh = 34;
	for (int i = 0; i < (int)buttons.size(); ++i) {
		const int w = Canvas::TextWidth(buttons[i].c_str()) + 28;
		Canvas::Bar(bx, by, w, bh, SELECT_BAR, i == button ? 120 : 35);
		Canvas::Text(bx + 14, by + 5, buttons[i].c_str(), i == button ? SELECTED : LABEL);
		bx += w + 8;
	}
	const size_t statusChars = (size_t)std::max(0, RIGHT - 20 - bx - 16) / Canvas::CharWidth();
	const std::string shown = Clip(status, statusChars);
	Canvas::Text(RIGHT - 20 - Canvas::TextWidth(shown.c_str()), by + 5, shown.c_str(), DIM);

	const int gridY = panelY + 52;
	if (games.empty()) {
		const int y = gridY + 30;
		Canvas::Text(LEFT + 24, y, "No games found.", TITLE, true);
		Canvas::Text(LEFT + 24, y + 80, "Put your ISO or CSO files in:", LABEL);
		Canvas::Text(LEFT + 24, y + 114, emptyFolder, SELECTED);
		Canvas::Text(LEFT + 24, y + 180, "Then choose Refresh above.", LABEL);
	} else {
		const int cellW = (RIGHT - LEFT - 16) / GRID_COLUMNS, cellH = 158;
		const int gridX = LEFT + 8;
		const size_t titleChars = (size_t)(cellW - 8) / Canvas::CharWidth();
		for (int row = 0; row < GRID_ROWS; ++row) {
			for (int col = 0; col < GRID_COLUMNS; ++col) {
				const int i = (topRow + row) * GRID_COLUMNS + col;
				if (i < 0 || i >= (int)games.size())
					continue;
				const GameTile &game = games[i];
				const int cx = gridX + col * cellW, cy = gridY + row * cellH;
				const int ix = cx + (cellW - ICON_W) / 2, iy = cy + 6;
				const bool sel = i == selection;
				if (sel)
					Canvas::Bar(cx + 2, cy, cellW - 4, cellH - 1, SELECT_BAR, 95);
				if (game.icon) {
					Canvas::Image(ix, iy, ICON_W, ICON_H, game.icon);
				} else {
					Canvas::Panel(ix, iy, ICON_W, ICON_H, 100);
					const char *kind = game.kind.c_str();
					Canvas::Text(ix + (ICON_W - Canvas::TextWidth(kind, true)) / 2, iy + (ICON_H - 48) / 2, kind, game.loading ? DISABLED : DIM, true);
				}
				if (sel)
					Canvas::Outline(ix - 3, iy - 3, ICON_W + 6, ICON_H + 6, 3, SELECTED, 230);
				const std::string title = Clip(game.title, titleChars);
				Canvas::Text(cx + (cellW - Canvas::TextWidth(title.c_str())) / 2, iy + ICON_H + 6, title.c_str(), sel ? SELECTED : VALUE);
			}
		}
	}
	const int infoY = 630;
	Canvas::Panel(LEFT, infoY, RIGHT - LEFT, 50, 125);
	const size_t chars = (size_t)(RIGHT - LEFT - 32) / Canvas::CharWidth();
	Canvas::Text(LEFT + 16, infoY + 13, Clip(hint, chars).c_str(), DIM);
}

size_t ReportVisibleLines() { return 17; }

void Popup(const std::string &title, const std::string &text, const std::vector<std::string> &items, int selection, const std::string &hint) {
	// Darkens what's behind it.
	Canvas::Panel(0, 0, Canvas::W, Canvas::H, 110);
	const int w = 680, rowH = 38;
	const size_t chars = (size_t)(w - 48) / Canvas::CharWidth();
	std::vector<std::string> lines = text.empty() ? std::vector<std::string>() : Wrap(text, chars);
	if (lines.size() > 4)
		lines.resize(4);
	const int h = 62 + (int)lines.size() * 27 + (lines.empty() ? 0 : 12) + (int)items.size() * rowH + 44;
	const int x = (Canvas::W - w) / 2, y = (Canvas::H - h) / 2;
	Canvas::Panel(x, y, w, h, 200);
	Canvas::Text(x + 24, y + 18, Clip(title, chars).c_str(), ACCENT);
	int cy = y + 56;
	for (const auto &line : lines) {
		Canvas::Text(x + 24, cy, line.c_str(), LABEL);
		cy += 27;
	}
	if (!lines.empty())
		cy += 12;
	for (int i = 0; i < (int)items.size(); ++i) {
		const bool sel = i == selection;
		if (sel)
			Canvas::Bar(x + 14, cy - 6, w - 28, rowH - 4, SELECT_BAR, 110);
		Canvas::Text(x + 32, cy, Clip(items[i], chars - 1).c_str(), sel ? SELECTED : VALUE);
		cy += rowH;
	}
	Canvas::Text(x + 24, y + h - 34, Clip(hint, chars).c_str(), DIM);
}

void Report(double seconds, const char *title, const std::vector<std::string> &lines, size_t top, size_t visible, const char *note) {
	Header(seconds, title, note);
	const int panelY = 96, rowH = 28;
	Canvas::Panel(LEFT, panelY, RIGHT - LEFT, (int)visible * rowH + 24, 135);
	for (size_t i = top; i < lines.size() && i < top + visible; ++i) {
		const std::string &line = lines[i];
		const bool fail = line.compare(0, 4, "FAIL") == 0, pass = line.compare(0, 4, "PASS") == 0;
		Canvas::Text(LEFT + 16, panelY + 12 + (int)(i - top) * rowH, Clip(line, 96).c_str(), fail ? 0xFF7A6E : (pass ? 0x86F0A0 : VALUE));
	}
	Canvas::Text(LEFT + 16, 650, "Up/Down: scroll   Left/Right: page   X/Circle: back", DIM);
}

void Viewer(double seconds, const char *title, const char *note, const std::vector<std::string> &lines, size_t top, size_t visible,
	int selected, const std::string &status, const std::string &hint) {
	Header(seconds, title, note);
	const int panelY = 96, rowH = 28;
	Canvas::Panel(LEFT, panelY, RIGHT - LEFT, (int)visible * rowH + 24, 135);
	for (size_t i = top; i < lines.size() && i < top + visible; ++i) {
		const int y = panelY + 12 + (int)(i - top) * rowH;
		const std::string &line = lines[i];
		if ((int)i == selected)
			Canvas::Bar(LEFT + 4, y - 2, RIGHT - LEFT - 8, rowH - 2, SELECT_BAR, 60);
		const size_t nb = line.find_first_not_of(' ');
		const bool comment = nb != std::string::npos && (line[nb] == '#' || line[nb] == ';');
		Canvas::Text(LEFT + 16, y, Clip(line, 96).c_str(), (int)i == selected ? SELECTED : (comment ? DIM : VALUE));
	}
	Canvas::Text(LEFT + 16, 622, Clip(status, 94).c_str(), LABEL);
	Canvas::Text(LEFT + 16, 654, Clip(hint, 94).c_str(), DIM);
}

void Message(double seconds, const char *title, const std::string &message, const std::string &footer1, const std::string &footer2) {
	Header(seconds, title, "");
	Canvas::Panel(LEFT, 150, RIGHT - LEFT, 240, 125);
	std::vector<std::string> lines = Wrap(message, (size_t)(RIGHT - LEFT - 48) / 12);
	for (size_t i = 0; i < lines.size() && i < 6; ++i)
		Canvas::Text(LEFT + 24, 180 + (int)i * 34, lines[i].c_str(), VALUE);
	Canvas::Panel(LEFT, 560, RIGHT - LEFT, 100, 125);
	Canvas::Text(LEFT + 16, 576, Clip(footer1, 94).c_str(), LABEL);
	Canvas::Text(LEFT + 16, 612, Clip(footer2, 94).c_str(), DIM);
}

void Editor(double seconds, const char *title, const char *note, const std::vector<std::string> &lines, int top, int curLine, int curCol,
	const std::vector<std::vector<std::string>> &keys, int keyRow, int keyCol, const std::string &status, const std::string &hint) {
	Header(seconds, title, note);
	const int panelY = 84, rowH = 30, textX = LEFT + 16;
	Canvas::Panel(LEFT, panelY, RIGHT - LEFT, EDITOR_VISIBLE_LINES * rowH + 20, 135);
	for (int i = 0; i < EDITOR_VISIBLE_LINES && top + i < (int)lines.size(); ++i) {
		const int line = top + i;
		const int y = panelY + 10 + i * rowH;
		std::string text = lines[line];
		// The cursor's line scrolls sideways to keep the cursor in view.
		int first = 0;
		if (line == curLine && curCol >= EDITOR_COLUMNS)
			first = curCol - EDITOR_COLUMNS + 1;
		text = first < (int)text.size() ? text.substr(first, EDITOR_COLUMNS) : std::string();
		if (line == curLine)
			Canvas::Bar(LEFT + 4, y, RIGHT - LEFT - 8, rowH - 2, SELECT_BAR, 40);
		const size_t nb = lines[line].find_first_not_of(' ');
		const bool comment = nb != std::string::npos && (lines[line][nb] == '#' || lines[line][nb] == ';');
		Canvas::Text(textX, y + 2, text.c_str(), comment ? DIM : VALUE);
		// A plain blinking text cursor: on for half a second, off for half a second.
		if (line == curLine && fmod(seconds, 1.0) < 0.5)
			Canvas::Fill(textX + (curCol - first) * Canvas::CharWidth() - 1, y + 3, 2, rowH - 8, VALUE);
	}
	// Keyboard.
	const int kbY = panelY + EDITOR_VISIBLE_LINES * rowH + 32, keyH = 40, gap = 6;
	Canvas::Panel(LEFT, kbY, RIGHT - LEFT, (int)keys.size() * (keyH + gap) + 18, 125);
	for (size_t r = 0; r < keys.size(); ++r) {
		const int n = (int)keys[r].size();
		if (n == 0)
			continue;
		const int rowW = RIGHT - LEFT - 24;
		const int keyW = (rowW - (n - 1) * gap) / n;
		const int y = kbY + 10 + (int)r * (keyH + gap);
		for (int k = 0; k < n; ++k) {
			const int x = LEFT + 12 + k * (keyW + gap);
			const bool sel = (int)r == keyRow && k == keyCol;
			Canvas::Bar(x, y, keyW, keyH, sel ? SELECT_BAR : 0x2A3A52, sel ? 200 : 150);
			const std::string &label = keys[r][k];
			const int tw = Canvas::TextWidth(label.c_str());
			Canvas::Text(x + (keyW - tw) / 2, y + (keyH - 24) / 2, label.c_str(), sel ? SELECTED : VALUE);
		}
	}
	Canvas::Text(LEFT + 16, 652, Clip(status, 94).c_str(), LABEL);
	Canvas::Text(LEFT + 16, 684, Clip(hint, 94).c_str(), DIM);
}

void Splash(double seconds, const char *text) {
	Canvas::Background(seconds);
	Canvas::Text((Canvas::W - Canvas::TextWidth(text, true)) / 2, (Canvas::H - 48) / 2, text, VALUE, true);
}

}  // namespace MenuScreens
