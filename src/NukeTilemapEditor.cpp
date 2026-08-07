// NukeTilemapEditor — editor-only companion of NukeTilemap: the .nutile tile set editor,
// registered via RegisterAssetEditor. Imports NukeImGui, so packaging excludes it from dists.
#include <interface/NUKEEInteface.h>
#include <interface/AppInstance.h>
#include <interface/AssetCreators.h>
#include <API/Model/Texture.h>
#include <API/Model/Jobs.h>   // RunOnMain: the window push must land on the game thread
#include <render/irender.h>
#include <NukeTilemap/Tilemap.h>
#include <nlohmann/json.hpp>

#include <imgui/imgui.h>
#include <nukeui.h>   // NukeUI::DocWindow — detachable document windows

#include <boost/filesystem.hpp>
#include <boost/filesystem/fstream.hpp>
#include <algorithm>
#include <array>
#include <iterator>
#include <string>
#include <vector>
#include <iostream>
#include <cstring>

using namespace nuke;
using json = nlohmann::json;
namespace bfs = boost::filesystem;

namespace {

bool g_windowPushed = false;   // touched on the game thread only (RunOnMain push, Shutdown pop)

struct TileDefEd
{
	int              id = 1;
	std::string      name;
	std::vector<int> cells;
	std::vector<std::array<int, 5>> rects;   // {x,y,w,h,rot}; non-empty overrides `cells`
	int              walk = 100;
	std::string      flags;   // comma-separated in the UI, array in the file
};

struct TileDoc
{
	std::string path;                    // full file path
	bool  open = true, dirty = false, wantFocus = true, confirmClose = false;
	std::string texPath;                 // content-relative atlas texture
	std::string normalPath;              // optional normal map (lit tiles); "" = unlit
	bool        normalDx = false;        // green convention: false = OpenGL (flip), true = DirectX
	int   cols = 4, rows = 4;
	std::vector<TileDefEd> tiles;
	int   sel = 0;
	uint64_t prev = 0; int prevW = 0, prevH = 0;   // atlas preview, GPU handle owned by this doc
	std::string prevFrom;                // texPath the preview was uploaded from
	float zoom = 1.0f;
	std::vector<std::string> undo, redo;  // whole-doc JSON snapshots, one per committed edit
	std::string idle;                    // pre-edit baseline
};

std::vector<TileDoc> g_docs;

// ---- (de)serialization: must match the JSON contract NukeTilemap's LoadTileSet parses ----

std::string DocJson(const TileDoc& d)
{
	json j;
	j["texture"] = d.texPath;
	if (!d.normalPath.empty()) { j["normal"] = d.normalPath; if (d.normalDx) j["normalDx"] = true; }
	j["cols"] = d.cols; j["rows"] = d.rows;
	json arr = json::array();
	for (const TileDefEd& t : d.tiles)
	{
		json e;
		e["id"] = t.id;
		e["name"] = t.name;
		e["cells"] = t.cells.empty() ? std::vector<int>{ 0 } : t.cells;
		if (!t.rects.empty())
		{
			json rs = json::array();
			for (const auto& r : t.rects)
			{
				json rr = json::array({ r[0], r[1], r[2], r[3] });
				if (r[4]) rr.push_back(1);
				rs.push_back(rr);
			}
			e["rects"] = rs;
		}
		e["walk"] = t.walk;
		json fl = json::array();
		std::string cur;
		for (char c : t.flags + ",")
		{
			if (c == ',' || c == ' ')
			{ if (!cur.empty()) fl.push_back(cur); cur.clear(); }
			else cur += c;
		}
		e["flags"] = fl;
		arr.push_back(e);
	}
	j["tiles"] = arr;
	return j.dump(2);
}

void ParseDoc(TileDoc& d, const std::string& text)
{
	d.tiles.clear();
	json j = json::parse(text, nullptr, false, true);
	if (j.is_discarded() || !j.is_object()) return;
	d.texPath = j.value("texture", std::string());
	d.normalPath = j.value("normal", std::string());
	d.normalDx   = j.value("normalDx", false);
	d.cols = std::max(1, j.value("cols", 4));
	d.rows = std::max(1, j.value("rows", 4));
	if (j.contains("tiles") && j["tiles"].is_array())
		for (const json& e : j["tiles"])
		{
			TileDefEd t;
			t.id   = e.value("id", 1);
			t.name = e.value("name", std::string());
			t.walk = e.value("walk", 100);
			if (e.contains("cells") && e["cells"].is_array())
				for (const json& c : e["cells"]) t.cells.push_back(c.get<int>());
			if (e.contains("rects") && e["rects"].is_array())
				for (const json& rj : e["rects"])
					if (rj.is_array() && rj.size() >= 4)
						t.rects.push_back({ rj[0].get<int>(), rj[1].get<int>(), rj[2].get<int>(),
						                    rj[3].get<int>(), rj.size() >= 5 && rj[4].get<int>() != 0 ? 1 : 0 });
			if (e.contains("flags") && e["flags"].is_array())
				for (const json& f : e["flags"])
				{ if (!t.flags.empty()) t.flags += ", "; t.flags += f.get<std::string>(); }
			d.tiles.push_back(std::move(t));
		}
	if (d.sel >= (int)d.tiles.size()) d.sel = d.tiles.empty() ? 0 : (int)d.tiles.size() - 1;
}

// ---- atlas preview ----

void FreePreview(TileDoc& d)
{
	if (d.prev)
		if (iRender* r = AppInstance::GetSingleton()->render)
			r->destroyTexture2D(d.prev);
	d.prev = 0; d.prevW = d.prevH = 0; d.prevFrom.clear();
}

void EnsurePreview(TileDoc& d)
{
	if (d.prev && d.prevFrom == d.texPath) return;
	FreePreview(d);
	d.prevFrom = d.texPath;
	if (d.texPath.empty()) return;
	iRender* r = AppInstance::GetSingleton()->render;
	if (!r) return;
	const std::string full = AppInstance::GetSingleton()->ResolveContent(d.texPath);
	nuke::Texture* t = nuke::Texture::LoadFromFile(full);
	if (!t) return;
	std::vector<unsigned char> rgba = t->DecodeRGBA();
	if (!rgba.empty() && t->width > 0 && t->height > 0 && t->width <= 8192 && t->height <= 8192)
	{
		d.prev = r->createTexture2D(rgba.data(), t->width, t->height);
		d.prevW = t->width; d.prevH = t->height;
	}
	delete t;
}

// ---- undo ----

void PushUndo(TileDoc& d)
{
	d.undo.push_back(d.idle);
	d.redo.clear();
	d.idle = DocJson(d);
	d.dirty = true;
}

void ApplySnapshot(TileDoc& d, const std::string& js)
{
	ParseDoc(d, js);
	d.idle = js;
	d.dirty = true;
	EnsurePreview(d);
}

// ---- save ----

void SaveDoc(TileDoc& d)
{
	bfs::ofstream f(bfs::path(d.path), std::ios::binary | std::ios::trunc);
	if (!f) { std::cout << "[TileEd]\tcannot write " << d.path << std::endl; return; }
	const std::string js = DocJson(d);
	f.write(js.data(), (std::streamsize)js.size());
	f.close();
	d.dirty = false;
	// Hot-reload the runtime module's cached set so live tilemaps rebake.
	boost::system::error_code ec;
	const std::string content = AppInstance::GetSingleton()->contentRoot;
	std::string rel = d.path;
	if (!content.empty())
	{
		bfs::path r = bfs::relative(bfs::path(d.path), bfs::path(content), ec);
		if (!ec && !r.empty() && r.generic_string().compare(0, 2, "..") != 0) rel = r.generic_string();
	}
	nuke::InvalidateTileSet(rel);
	std::cout << "[TileEd]\tsaved " << d.path << std::endl;
}

// ---- the window ----

// Content .nutex picker: button with the current file, popup lists every .nutex under the
// project content. Returns true when the selection changed.
static bool NutexPicker(const char* label, std::string& path, float width)
{
	namespace bfs = boost::filesystem;
	bool changed = false;
	ImGui::PushID(label);
	std::string cur = path.empty() ? "(none)" : bfs::path(path).stem().string();
	if (ImGui::Button((cur + "##pick").c_str(), ImVec2(width, 0))) ImGui::OpenPopup("##nutexpop");
	ImGui::SameLine(0, 6); ImGui::TextUnformatted(label);
	if (ImGui::BeginPopup("##nutexpop"))
	{
		ImGui::BeginChild("##lst", ImVec2(320, 240));
		if (ImGui::Selectable("(none)", path.empty())) { path.clear(); changed = true; ImGui::CloseCurrentPopup(); }
		boost::system::error_code ec;
		bfs::path croot(AppInstance::GetSingleton()->contentRoot);
		if (bfs::exists(croot, ec))
			for (bfs::recursive_directory_iterator it(croot, ec), end; it != end; it.increment(ec))
			{
				if (ec) break;
				if (bfs::is_directory(it->path()) || it->path().extension() != ".nutex") continue;
				std::string rel = bfs::relative(it->path(), croot, ec).generic_string();
				if (ImGui::Selectable((rel + "##" + rel).c_str(), rel == path))
				{ path = rel; changed = true; ImGui::CloseCurrentPopup(); }
			}
		ImGui::EndChild();
		ImGui::EndPopup();
	}
	ImGui::PopID();
	return changed;
}

static void DrawDocBody(TileDoc& d);

void DrawDoc(TileDoc& d)
{
	EnsurePreview(d);
	// The content lambda re-finds the doc BY PATH: it runs in the host pass, where g_docs may
	// have shifted underneath a captured reference.
	const std::string docId = "tile:" + d.path;
	// "###<id>" keeps the imgui identity stable while the dirty " *" comes and goes.
	const std::string title = bfs::path(d.path).filename().string() + (d.dirty ? " *" : "") + "###" + docId;
	if (d.wantFocus) { NukeUI::DocFocus(docId.c_str()); d.wantFocus = false; }
	bool keep = true;
	const std::string keyPath = d.path;
	NukeUI::DocWindow(docId.c_str(), title.c_str(), &keep, 0, 860, 620, [keyPath]()
	{
		for (TileDoc& dd : g_docs)
			if (dd.path == keyPath) { DrawDocBody(dd); return; }
	});
	if (!keep) d.confirmClose = true;
}

static void DrawDocBody(TileDoc& d)
{
	// ---- toolbar ----
	if (ImGui::Button("Save") || (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows)
	    && ImGui::GetIO().KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_S, false)))
		SaveDoc(d);
	ImGui::SameLine();
	if (ImGui::Button("Revert"))
	{
		bfs::ifstream f{ bfs::path(d.path) };
		std::string js((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
		ParseDoc(d, js);
		d.idle = DocJson(d); d.undo.clear(); d.redo.clear(); d.dirty = false;
		EnsurePreview(d);
	}
	if (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) && ImGui::GetIO().KeyCtrl)
	{
		if (ImGui::IsKeyPressed(ImGuiKey_Z, false) && !d.undo.empty())
		{ d.redo.push_back(DocJson(d)); std::string s = d.undo.back(); d.undo.pop_back(); ApplySnapshot(d, s); }
		if (ImGui::IsKeyPressed(ImGuiKey_Y, false) && !d.redo.empty())
		{ d.undo.push_back(DocJson(d)); std::string s = d.redo.back(); d.redo.pop_back(); ApplySnapshot(d, s); }
	}
	ImGui::SameLine();
	ImGui::TextDisabled("atlas: click a cell to toggle it on the selected tile");
	ImGui::Separator();

	// ---- texture + grid row ----
	{
		if (NutexPicker("Texture", d.texPath, 280)) PushUndo(d);
		if (NutexPicker("Normal", d.normalPath, 280)) PushUndo(d);   // set -> tiles draw LIT
		ImGui::SameLine();
		bool dx = d.normalDx;
		if (ImGui::Checkbox("DirectX green", &dx)) { d.normalDx = dx; PushUndo(d); }
		if (ImGui::IsItemHovered()) ImGui::SetTooltip("Off = OpenGL convention (+Y up, green flipped - import default)");
		if (ImGui::IsItemHovered()) ImGui::SetTooltip("Content-relative .nutex the atlas cells come from");
		ImGui::SameLine();
		int cr[2] = { d.cols, d.rows };
		ImGui::SetNextItemWidth(120);
		if (ImGui::InputInt2("Cols/Rows", cr))
		{
			d.cols = std::max(1, std::min(256, cr[0]));
			d.rows = std::max(1, std::min(256, cr[1]));
			PushUndo(d);
		}
		ImGui::SameLine();
		ImGui::SetNextItemWidth(110);
		ImGui::SliderFloat("Zoom", &d.zoom, 0.25f, 4.0f, "%.2fx");
	}

	// ---- left: tile list ----
	const float listW = 200.0f;
	ImGui::BeginChild("##tilelist", ImVec2(listW, 0), ImGuiChildFlags_Borders);
	if (ImGui::Button("+ Add", ImVec2(-1, 0)))
	{
		int nextId = 1;
		for (const TileDefEd& t : d.tiles) nextId = std::max(nextId, t.id + 1);
		TileDefEd t; t.id = nextId; t.name = "tile" + std::to_string(nextId); t.cells = { 0 };
		d.tiles.push_back(std::move(t));
		d.sel = (int)d.tiles.size() - 1;
		PushUndo(d);
	}
	for (int i = 0; i < (int)d.tiles.size(); ++i)
	{
		ImGui::PushID(i);
		char lbl[96];
		snprintf(lbl, sizeof(lbl), "%d  %s", d.tiles[i].id, d.tiles[i].name.c_str());
		if (ImGui::Selectable(lbl, d.sel == i)) d.sel = i;
		ImGui::PopID();
	}
	ImGui::EndChild();
	ImGui::SameLine();

	// ---- right: selected tile fields + the atlas ----
	ImGui::BeginChild("##tileright", ImVec2(0, 0));
	if (d.sel >= 0 && d.sel < (int)d.tiles.size())
	{
		TileDefEd& t = d.tiles[d.sel];
		ImGui::SetNextItemWidth(90);
		int id = t.id;
		if (ImGui::InputInt("Id", &id, 0) && id > 0 && id != t.id)
		{
			bool taken = false;
			for (const TileDefEd& o : d.tiles) if (&o != &t && o.id == id) taken = true;
			if (!taken) { t.id = id; PushUndo(d); }
		}
		if (ImGui::IsItemHovered()) ImGui::SetTooltip("Cell value stored in the map (unique; 0 = empty is reserved)");
		ImGui::SameLine();
		char nm[128]; strncpy(nm, t.name.c_str(), 127); nm[127] = 0;
		ImGui::SetNextItemWidth(160);
		if (ImGui::InputText("Name", nm, sizeof(nm), ImGuiInputTextFlags_EnterReturnsTrue)) { t.name = nm; PushUndo(d); }
		ImGui::SameLine();
		int walk = t.walk;
		ImGui::SetNextItemWidth(90);
		if (ImGui::InputInt("Walk", &walk, 0)) { t.walk = std::max(0, walk); PushUndo(d); }
		if (ImGui::IsItemHovered()) ImGui::SetTooltip("Move cost: 100 = normal ground, bigger = slower, 0 = impassable (wall)");
		ImGui::SameLine();
		char fl[256]; strncpy(fl, t.flags.c_str(), 255); fl[255] = 0;
		ImGui::SetNextItemWidth(-60);
		if (ImGui::InputText("Flags", fl, sizeof(fl), ImGuiInputTextFlags_EnterReturnsTrue)) { t.flags = fl; PushUndo(d); }
		if (ImGui::IsItemHovered()) ImGui::SetTooltip("Comma-separated (wall, mine, plant, buildable, ...) — the game reads them as a bitmask");
		ImGui::SameLine();
		if (ImGui::Button("Delete"))
		{
			d.tiles.erase(d.tiles.begin() + d.sel);
			if (d.sel >= (int)d.tiles.size()) d.sel = (int)d.tiles.size() - 1;
			PushUndo(d);
			ImGui::EndChild(); ImGui::End(); return;   // list mutated: redraw next frame
		}
		if (t.rects.empty())
		{
			std::string cellsTxt = "Cells:";
			for (int c : t.cells) cellsTxt += " " + std::to_string(c);
			ImGui::TextDisabled("%s   (several = visual variants; mapgen picks per cell)", cellsTxt.c_str());
		}
		else
		{
			ImGui::TextDisabled("Rects (px, override the grid; several = visual variants):");
			int kill = -1;
			for (int ri = 0; ri < (int)t.rects.size(); ++ri)
			{
				ImGui::PushID(1000 + ri);
				int v[4] = { t.rects[ri][0], t.rects[ri][1], t.rects[ri][2], t.rects[ri][3] };
				ImGui::SetNextItemWidth(260);
				if (ImGui::InputInt4("##xywh", v))
				{
					t.rects[ri][0] = v[0]; t.rects[ri][1] = v[1];
					t.rects[ri][2] = std::max(1, v[2]); t.rects[ri][3] = std::max(1, v[3]);
					PushUndo(d);
				}
				ImGui::SameLine();
				bool rot = t.rects[ri][4] != 0;
				if (ImGui::Checkbox("rot", &rot)) { t.rects[ri][4] = rot ? 1 : 0; PushUndo(d); }
				if (ImGui::IsItemHovered()) ImGui::SetTooltip("Region stored rotated 90\xC2\xB0 clockwise in the page");
				ImGui::SameLine();
				if (ImGui::SmallButton("x")) kill = ri;
				ImGui::PopID();
			}
			if (kill >= 0) { t.rects.erase(t.rects.begin() + kill); PushUndo(d); }
			if (ImGui::SmallButton("+ rect")) { t.rects.push_back({ 0, 0, 64, 64, 0 }); PushUndo(d); }
		}
	}
	else
		ImGui::TextDisabled("No tiles — add one on the left.");
	ImGui::Separator();

	// ---- the clickable atlas ----
	ImGui::BeginChild("##atlas", ImVec2(0, 0), ImGuiChildFlags_Borders,
	                  ImGuiWindowFlags_HorizontalScrollbar);
	if (!d.prev)
		ImGui::TextDisabled("Texture not found: '%s' (set a content-relative .nutex above)", d.texPath.c_str());
	else
	{
		const float w = (float)d.prevW * d.zoom, h = (float)d.prevH * d.zoom;
		ImVec2 p0 = ImGui::GetCursorScreenPos();
		ImGui::Image((ImTextureID)d.prev, ImVec2(w, h));
		ImDrawList* dl = ImGui::GetWindowDrawList();
		const float cw = w / d.cols, ch = h / d.rows;
		// grid
		for (int c = 0; c <= d.cols; ++c)
			dl->AddLine(ImVec2(p0.x + c * cw, p0.y), ImVec2(p0.x + c * cw, p0.y + h), IM_COL32(255, 255, 255, 70));
		for (int r = 0; r <= d.rows; ++r)
			dl->AddLine(ImVec2(p0.x, p0.y + r * ch), ImVec2(p0.x + w, p0.y + r * ch), IM_COL32(255, 255, 255, 70));
		// free-form rects: selected tile green, others dim blue
		for (int i = 0; i < (int)d.tiles.size(); ++i)
			for (const auto& rc : d.tiles[i].rects)
			{
				const float sx = (float)d.prevW ? w / (float)d.prevW : 1.0f;
				const float sy = (float)d.prevH ? h / (float)d.prevH : 1.0f;
				const float rw = (rc[4] ? rc[3] : rc[2]) * sx;   // rotated: page span swaps w/h
				const float rh = (rc[4] ? rc[2] : rc[3]) * sy;
				ImVec2 a(p0.x + rc[0] * sx, p0.y + rc[1] * sy), b(a.x + rw, a.y + rh);
				if (i == d.sel)
				{
					dl->AddRectFilled(a, b, IM_COL32(80, 255, 120, 45));
					dl->AddRect(a, b, IM_COL32(80, 255, 120, 220), 0, 0, 2.0f);
				}
				else
					dl->AddRect(a, b, IM_COL32(80, 140, 255, 90));
			}
		// cell ownership tints
		for (int i = 0; i < (int)d.tiles.size(); ++i)
			for (int cell : d.tiles[i].cells)
			{
				if (!d.tiles[i].rects.empty()) continue;   // rect tiles: cells are a dormant fallback
				const int cx = cell % d.cols, cy = cell / d.cols;
				if (cy >= d.rows) continue;
				ImVec2 a(p0.x + cx * cw, p0.y + cy * ch), b(p0.x + (cx + 1) * cw, p0.y + (cy + 1) * ch);
				dl->AddRectFilled(a, b, i == d.sel ? IM_COL32(80, 255, 120, 60) : IM_COL32(80, 140, 255, 35));
				if (i == d.sel) dl->AddRect(a, b, IM_COL32(80, 255, 120, 220), 0, 0, 2.0f);
			}
		// hover + click
		if (ImGui::IsItemHovered())
		{
			ImVec2 m = ImGui::GetMousePos();
			const int cx = (int)((m.x - p0.x) / cw), cy = (int)((m.y - p0.y) / ch);
			if (cx >= 0 && cy >= 0 && cx < d.cols && cy < d.rows)
			{
				const int cell = cy * d.cols + cx;
				dl->AddRect(ImVec2(p0.x + cx * cw, p0.y + cy * ch),
				            ImVec2(p0.x + (cx + 1) * cw, p0.y + (cy + 1) * ch), IM_COL32(255, 220, 80, 255));
				std::string owners;
				for (const TileDefEd& t : d.tiles)
					if (std::find(t.cells.begin(), t.cells.end(), cell) != t.cells.end())
					{ if (!owners.empty()) owners += ", "; owners += t.name; }
				ImGui::SetTooltip("cell %d%s%s", cell, owners.empty() ? "" : " — ", owners.c_str());
				// grid-cell toggling applies to grid tiles only; rect tiles are edited numerically
				if (ImGui::IsMouseClicked(ImGuiMouseButton_Left) && d.sel >= 0 && d.sel < (int)d.tiles.size()
				    && d.tiles[d.sel].rects.empty())
				{
					auto& cells = d.tiles[d.sel].cells;
					auto it = std::find(cells.begin(), cells.end(), cell);
					if (it != cells.end()) cells.erase(it); else cells.push_back(cell);
					PushUndo(d);
				}
			}
		}
	}
	ImGui::EndChild();
	ImGui::EndChild();

	// ---- dirty close guard ----
	if (d.confirmClose)
	{
		if (!d.dirty) { d.open = false; d.confirmClose = false; }
		else ImGui::OpenPopup(("Unsaved changes###tileclose:" + d.path).c_str());
	}
	if (ImGui::BeginPopupModal(("Unsaved changes###tileclose:" + d.path).c_str(), nullptr,
	                           ImGuiWindowFlags_AlwaysAutoResize))
	{
		ImGui::Text("'%s' has unsaved changes.", bfs::path(d.path).filename().string().c_str());
		if (ImGui::Button("Save"))    { SaveDoc(d); d.open = false; ImGui::CloseCurrentPopup(); }
		ImGui::SameLine();
		if (ImGui::Button("Discard")) { d.open = false; ImGui::CloseCurrentPopup(); }
		ImGui::SameLine();
		if (ImGui::Button("Cancel"))  { d.confirmClose = false; ImGui::CloseCurrentPopup(); }
		ImGui::EndPopup();
	}
}

void DrawAllDocs()
{
	for (TileDoc& d : g_docs)
		if (d.open) DrawDoc(d);
	for (size_t i = 0; i < g_docs.size(); )
	{
		if (!g_docs[i].open) { FreePreview(g_docs[i]); g_docs.erase(g_docs.begin() + i); }
		else ++i;
	}
}

void OpenTileDoc(const std::string& path)
{
	// De-dup by the CANONICAL file, not the string: the same file arrives spelled differently
	// (slashes, relative vs absolute), and two docs for one file fight over it.
	auto norm = [](const std::string& p)
	{
		boost::system::error_code ec;
		bfs::path c = bfs::weakly_canonical(bfs::path(p), ec);
		std::string s = (ec ? bfs::path(p) : c).generic_string();
		for (char& ch : s) ch = (char)std::tolower((unsigned char)ch);
		return s;
	};
	const std::string key = norm(path);
	for (TileDoc& d : g_docs)
		if (norm(d.path) == key)
		{
			std::cout << "[NukeTilemapEditor]\topen '" << path << "' -> already open, focusing" << std::endl;
			d.wantFocus = true; d.open = true; return;
		}
	std::cout << "[NukeTilemapEditor]\topen '" << path << "' (docs before=" << g_docs.size() << ")" << std::endl;
	TileDoc d;
	d.path = path;
	bfs::ifstream f{ bfs::path(path) };
	std::string js((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
	ParseDoc(d, js);
	d.idle = DocJson(d);
	g_docs.push_back(std::move(d));
}

}  // namespace

// ---- the module ----

class NukeTilemapEditorModule : public NUKEModule
{
public:
	NukeTilemapEditorModule()
	{
		strcpy(title, "NukeTilemapEditor");
		strcpy(author, "Luastris");
		strcpy(version, "1.0");
		strcpy(description, "Editor tooling for NukeTilemap: the .nutile Tile Set editor");
	}

	void OnLoad() override
	{
		RegisterAssetEditor(".nutile", [](const std::string& path) { OpenTileDoc(path); });
		std::cout << "[NukeTilemapEditor]\tloaded (.nutile editor registered)" << std::endl;
	}

	void Run(AppInstance* instance) override
	{
		if (!instance || !instance->isEditor()) return;   // tooling: editor host only
		// Run() is a background plugin thread; the window list is main-thread state -> RunOnMain.
		Jobs::RunOnMain([instance]()
		{
			if (g_windowPushed) return;
			instance->PushWindow("nuketilemap-editors", &DrawAllDocs);
			g_windowPushed = true;
		});
	}

	void Shutdown() override
	{
		// Runs on the main thread (DisablePlugin / UnloadModules) — safe to pop directly.
		if (g_windowPushed && instance) { instance->PopWindow("nuketilemap-editors"); g_windowPushed = false; }
		for (TileDoc& d : g_docs) FreePreview(d);
		g_docs.clear();
		stopped = true;
	}

	bool HasSettings() override { return false; }
	void Settings() override {}
	bool editorTool() override { return true; }
	const char* companionOf() override { return "NukeTilemap.dll"; }   // always on in the editor; never ships
};

// Exported under the unmangled symbol "plugin" — the loader imports it via boost::dll.
extern "C" BOOST_SYMBOL_EXPORT NukeTilemapEditorModule plugin;
NukeTilemapEditorModule plugin;
