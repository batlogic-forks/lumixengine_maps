#include "editor/platform_interface.h"
#include "editor/render_interface.h"
#include "editor/studio_app.h"
#include "editor/utils.h"
#include "editor/world_editor.h"
#include "engine/math_utils.h"
#include "engine/path_utils.h"
#include "imgui/imgui.h"
#include "stb/stb_image.h"
#include <Windows.h>
#include <WinInet.h>
#include <cmath>
#include <cstdlib>
#pragma comment(lib, "wininet.lib")


using namespace Lumix;


namespace
{


constexpr float M_PI = 3.14159265f;


int long2tilex(double lon, int z)
{
	return (int)(floor((lon + 180.0) / 360.0 * pow(2.0, z)));
}

int lat2tiley(double lat, int z)
{
	return (int)(floor((1.0 - log(tan(lat * M_PI / 180.0) + 1.0 / cos(lat * M_PI / 180.0)) / M_PI) / 2.0 * pow(2.0, z)));
}

double tilex2long(float x, int z)
{
	return x / pow(2.0, z) * 360.0 - 180;
}

double tiley2lat(float y, int z)
{
	double n = M_PI - 2.0 * M_PI * y / pow(2.0, z);
	return 180.0 / M_PI * atan(0.5 * (exp(n) - exp(-n)));
}


double googleTileLat(float y, int z)
{
	double lat = tiley2lat(y, z);
	double nlat = tiley2lat(y + 1, z);
	return lat; // (lat + nlat) * 0.5;
}


double googleTileLong(float x, int z)
{
	double lng = tilex2long(x, z);
	double nlng = tilex2long(x + 1, z);
	return lng; // (lng + nlng) * 0.5;
}


struct MapsPlugin LUMIX_FINAL : public StudioApp::IPlugin
{
	struct ImageData
	{
		ImTextureID texture = nullptr;
		u8* pixels = nullptr;
		int w;
		int h;
	};


	MapsPlugin(StudioApp& app)
		: m_app(app)
		, m_open(false)
	{
		Action* action = LUMIX_NEW(app.getWorldEditor()->getAllocator(), Action)("Maps", "maps");
		action->func.bind<MapsPlugin, &MapsPlugin::toggleOpen>(this);
		action->is_selected.bind<MapsPlugin, &MapsPlugin::isOpen>(this);
		app.addWindowAction(action);
		copyString(m_out_path, "out.raw");
	}


	void toggleOpen() { m_open = !m_open; }
	bool isOpen() const { return m_open; }


	bool downloadImage(const char* name, const char* url, ImageData* out)
	{
		char url_path[2048];
		URL_COMPONENTSA urlc;
		memset(&urlc, 0, sizeof(urlc));
		urlc.dwStructSize = sizeof(urlc);
		urlc.lpszUrlPath = url_path;
		urlc.dwUrlPathLength = sizeof(url_path);

		if (!InternetCrackUrlA(url, 0, ICU_DECODE, &urlc)) return false;

		HINTERNET inet = InternetOpen("LumixEngine", // user agent
			INTERNET_OPEN_TYPE_PRECONFIG,
			NULL, // lpszProxyName
			NULL, // lpszProxyBypass
			0	 // dwFlags
		);
		if (!inet) return false;

		DWORD dwFlags = 0;
		dwFlags |= INTERNET_FLAG_PRAGMA_NOCACHE | INTERNET_FLAG_RELOAD;
		dwFlags |= INTERNET_FLAG_SECURE;

		HINTERNET conn;

		HINTERNET conn_raw = InternetOpenUrlA(inet,
			url,
			NULL, // lpszHeaders
			-1,   // dwHeadersLength
			dwFlags,
			NULL // dwContext
		);
		if (conn_raw)
		{
			conn = conn_raw;
		}
		else
		{
			if (GetLastError() != ERROR_IO_PENDING) return false;
		}
		char buf[1024 * 10];
		DWORD read;
		auto err2 = GetLastError();
		BOOL res = TRUE;
		Array<u8> data(m_app.getWorldEditor()->getAllocator());
		data.reserve(4096);
		do
		{
			res = InternetReadFile(conn_raw, buf, sizeof(buf), &read);
			if (read > 0)
			{
				data.resize(data.size() + read);
				copyMemory(&data[data.size() - read], buf, read);
			}
			auto err = GetLastError();
		} while (read > 0);
		res = res;

		int channels;
		out->pixels = stbi_load_from_memory(&data[0], data.size(), &out->w, &out->h, &channels, 4);
		if (!out->pixels) return false;
		out->texture = m_app.getWorldEditor()->getRenderInterface()->createTexture(name, out->pixels, out->w, out->h);
		if (!out->texture)
		{
			stbi_image_free(out->pixels);
			return false;
		}


		return true;
	}


	void clear()
	{
		if (m_satallite_map.texture)
		{
			m_app.getWorldEditor()->getRenderInterface()->destroyTexture(m_satallite_map.texture);
			m_satallite_map.texture = nullptr;
		}
		if (m_height_map.texture)
		{
			m_app.getWorldEditor()->getRenderInterface()->destroyTexture(m_height_map.texture);
			m_height_map.texture = nullptr;
		}
		stbi_image_free(m_satallite_map.pixels);
		stbi_image_free(m_height_map.pixels);
		m_satallite_map.pixels = nullptr;
		m_height_map.pixels = nullptr;
	}


	void getHeightmapPath(char* url)
	{
		sprintf(url,
			"http://s3.amazonaws.com/elevation-tiles-prod/terrarium/%d/%d/%d.png",
			m_zoom,
			m_x,
			m_y);
	}



	void getSatellitePath(char* url)
	{
		double lng = googleTileLong(m_x + 0.5f, m_zoom);
		double lat = googleTileLat(m_y + 0.5f, m_zoom);

		sprintf(url,
			"https://maps.googleapis.com/maps/api/"
			"staticmap?center=%f,%f&zoom=%d&size=256x256&maptype=satellite&&key="
			"AIzaSyDVNwegaW4klzqUfqZ038zorEgao8TtNHs",
			lat,
			lng,
			m_zoom);
	}


	bool download()
	{
		m_is_download_deferred = false;
		clear();

		m_x = m_x % (1 << m_zoom);
		m_y = m_y % (1 << m_zoom);

		char url[1024];
		getSatellitePath(url);
		bool res = downloadImage("maps", url, &m_satallite_map);

		getHeightmapPath(url);
		res = downloadImage("maps_hm", url, &m_height_map) & res;
		
		if (!res) clear();
		return res;
	}


	~MapsPlugin()
	{
		clear();
	}


	union RGBA
	{
		struct {
			u8 r;
			u8 g;
			u8 b;
			u8 a;
		};
		u32 rgba;
	};


	void browse()
	{
		PlatformInterface::getSaveFilename(m_out_path, lengthOf(m_out_path), "Raw Image\0*.raw\0", "raw");
	}


	void save()
	{
		if (!m_height_map.pixels) return;
		if (!m_satallite_map.pixels) return;
		
		Array<u16> raw(m_app.getWorldEditor()->getAllocator());
		raw.resize(m_height_map.w * m_height_map.h);
		const RGBA* in = (const RGBA*)m_height_map.pixels;

		u32 min = 0xffffFFFF;
		u32 max = 0;
		for (int i = 0; i < m_height_map.w * m_height_map.h; ++i)
		{
			RGBA rgba = in[i];
			u32 p = u32((rgba.r << 16) + (rgba.g << 8) + rgba.b);
			if (max < p) max = p;
			if (min > p) min = p;
		}

		double diff = max - min;

		for (int i = 0; i < m_height_map.w * m_height_map.h; ++i)
		{
			RGBA rgba = in[i];
			u32 p = u32((rgba.r << 16) + (rgba.g << 8) + rgba.b);
			raw[i] = u16((double(p - min) / diff) * 0xffff);
		}

		FILE* fp = fopen(m_out_path, "wb");
		if (fp)
		{
			fwrite(&raw[0], raw.size() * 2, 1, fp);
			fclose(fp);
		}

		RenderInterface* ri = m_app.getWorldEditor()->getRenderInterface();
		PathUtils::FileInfo file_info(m_out_path);
		StaticString<MAX_PATH_LENGTH> tga_path(file_info.m_dir, "/", file_info.m_basename, ".tga");
		ri->saveTexture(m_app.getWorldEditor()->getEngine(), tga_path, m_satallite_map.pixels, m_satallite_map.w, m_satallite_map.h);

	}


	void onWindowGUI() override
	{
		if (!ImGui::BeginDock("Maps", &m_open))
		{
			ImGui::EndDock();
			return;
		}

		if (m_is_download_deferred) download();

		if (ImGui::Button("Refresh")) download();
		if (m_satallite_map.pixels)
		{
			ImGui::SameLine();
			if (ImGui::Button("Save")) save();
		}
		ImGui::SameLine();
		int zoom = m_zoom;
		if (ImGui::SliderInt("Zoom", &zoom, 0, 18))
		{
			if (zoom > m_zoom)
			{
				m_x <<= zoom - m_zoom;
				m_y <<= zoom - m_zoom;
				m_zoom = zoom;
			}
			else
			{
				m_x >>= m_zoom - zoom;
				m_y >>= m_zoom - zoom;
				m_zoom = zoom;
			}
			download();
		}
		ImGui::SameLine();
		if (ImGui::Button("+"))
		{
			++m_zoom;
			m_x <<= 1;
			m_y <<= 1;
			download();
		}
		ImGui::SameLine();
		if (ImGui::Button("-"))
		{
			--m_zoom;
			m_x >>= 1;
			m_y >>= 1;
			if (m_zoom < 0) m_zoom = 0;
			download();
		}
		ImGui::SameLine();
		if (ImGui::Button("<"))
		{
			--m_x;
			if (m_x < 0) m_x = 0;
			download();
		}
		ImGui::SameLine();
		if (ImGui::Button(">"))
		{
			++m_x;
			download();
		}

		ImGui::LabelText("Output", m_out_path);
		ImGui::SameLine();
		if (ImGui::Button("...")) browse();
		static bool show_hm = false;
		ImGui::Checkbox("Show HeightMap", &show_hm);
		ImVec2 cursor_pos = ImGui::GetCursorScreenPos();
		if (m_satallite_map.texture && !show_hm) ImGui::Image(m_satallite_map.texture, ImVec2(512, 512));
		if (m_height_map.texture && show_hm) ImGui::Image(m_height_map.texture, ImVec2(512, 512));

		if (ImGui::IsMouseClicked(0) && ImGui::IsItemHovered())
		{
			m_mouse_down_pos  = ImGui::GetMousePos();
		}
		if (ImGui::IsMouseReleased(0) && ImGui::IsItemHovered())
		{
			ImVec2 up_pos = ImGui::GetMousePos();
			float diff = Math::maximum(Math::abs(up_pos.x - m_mouse_down_pos.x), Math::abs(up_pos.y - m_mouse_down_pos.y));
			int new_zoom = m_zoom;
			while (diff * 2 < 256)
			{
				++new_zoom;
				diff *= 2;
			}
			if (new_zoom != m_zoom)
			{
				int x = m_x << (new_zoom - m_zoom);
				int y = m_y << (new_zoom - m_zoom);
				float left = Math::minimum(up_pos.x, m_mouse_down_pos.x) - cursor_pos.x;
				float up = Math::minimum(up_pos.y, m_mouse_down_pos.y) - cursor_pos.y;
				m_x = x + int((left / 512.0f) * (1 << (new_zoom - m_zoom)));
				m_y = y + int((up / 512.0f) * (1 << (new_zoom - m_zoom)));
				m_zoom = new_zoom;
				m_is_download_deferred = true;
			}
		}

		ImGui::Text("Uses https://aws.amazon.com/public-datasets/terrain/");
		ImGui::Text("http://s3.amazonaws.com/elevation-tiles-prod/terrarium/%d/%d/%d.png", m_zoom, m_x, m_y);

		ImGui::EndDock();
	}


	const char* getName() const override { return "maps"; }

	StudioApp& m_app;
	ImageData m_satallite_map;
	ImageData m_height_map;
	bool m_open = false;
	bool m_is_download_deferred = true;
	int m_zoom = 0;
	int m_x = 0;
	int m_y = 0;
	char m_out_path[MAX_PATH_LENGTH];
	ImVec2 m_mouse_down_pos;
};


} // anonoymous namespace


LUMIX_STUDIO_ENTRY(lumixengine_maps)
{
	WorldEditor& editor = *app.getWorldEditor();

	auto* plugin = LUMIX_NEW(editor.getAllocator(), MapsPlugin)(app);
	app.addPlugin(*plugin);
}

