#include "chams.h"
#include "cache.h"
#include "../../memory/memory.h"
#include "../../Roblox/offsets.h"
#include "../../sdk/math.h"
#include "../../overlay/overlay.h"

#include <clipper2/clipper.h>

#include <d3dcompiler.h>
#include <atomic>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <functional>
#include <string>
#include <vector>
#include <windows.h>

#include "imgui.h"

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "d3dcompiler.lib")

namespace
{
	constexpr char kShaderHLSL[] = R"HLSL(
cbuffer cb : register(b0)
{
    row_major float4x4 view;
    row_major float4x4 world;
    float3 camera;
    float  _pad_camera;
    float4 base_color;
    float4 fresnel_color;
    int    mode;
    float  fresnel_power;
    float  brightness;
    float  _pad_tail;
};

struct VSIn  { float3 pos : POSITION; float3 normal : NORMAL; };
struct PSIn  { float4 pos : SV_POSITION; float3 wpos : TEXCOORD0; float3 normal : NORMAL; };

PSIn vs_main(VSIn i)
{
    PSIn o;
    float4 wp = mul(world, float4(i.pos, 1.0));
    o.pos    = mul(view, wp);
    o.pos.z  = saturate(length(wp.xyz - camera) / 5000.0) * o.pos.w;
    o.wpos   = wp.xyz;
    o.normal = normalize(mul((float3x3)world, i.normal));
    return o;
}

float4 ps_main(PSIn i) : SV_TARGET
{
    float3 dn = cross(ddy(i.wpos), ddx(i.wpos));
    float3 N  = (dot(i.normal, i.normal) > 1e-6) ? normalize(i.normal) : normalize(dn);

    float3 V  = normalize(camera - i.wpos);
    float facing = dot(N, V);
    clip(facing - 0.001);

    float fres = pow(saturate(1.0 - facing), max(0.5, fresnel_power));

    float4 result = base_color;

    if (mode == 0)
    {
        result.rgb = base_color.rgb;
        result.a   = base_color.a;
    }
    else if (mode == 1)
    {
        const float3 sky    = float3(1.05, 1.05, 1.10);
        const float3 ground = float3(0.30, 0.32, 0.38);
        float  hemi    = saturate(N.y * 0.5 + 0.5);
        float3 ambient = lerp(ground, sky, hemi);

        float3 L   = normalize(float3(0.40, 0.85, 0.35));
        float  ndl = saturate(dot(N, L));
        float  half_l = ndl * 0.5 + 0.5;
        float  diffuse = half_l * half_l;

        float3 H   = normalize(L + V);
        float  spec = pow(saturate(dot(N, H)), 64.0);

        float3 lit = base_color.rgb * (ambient * 0.45 + diffuse * 0.65)
                   + spec * 0.35
                   + fresnel_color.rgb * fres * 0.30;

        result.rgb = lit;
        result.a   = base_color.a;
    }
    else if (mode == 2)
    {
        float core = 0.18;
        result.rgb = base_color.rgb * core + fresnel_color.rgb * fres;
        result.a   = saturate(core + fres) * base_color.a;
    }
    else if (mode == 3)
    {
        float3 R = reflect(-V, N);

        float3 envSky     = float3(0.85, 0.92, 1.10);
        float3 envGround  = float3(0.05, 0.06, 0.09);
        float3 envHorizon = float3(1.35, 1.22, 1.00);

        float horizon = exp(-R.y * R.y * 18.0);
        float3 env = lerp(envGround, envSky, smoothstep(-0.25, 0.50, R.y));
        env += envHorizon * horizon * 0.65;

        float F0 = 0.78;
        float fresMetal = F0 + (1.0 - F0) * pow(1.0 - facing, 5.0);

        float3 silver = float3(0.92, 0.94, 0.98);
        float3 tint   = lerp(silver, base_color.rgb, 0.40);

        float streak = pow(saturate(1.0 - abs(R.x * R.x - R.z * R.z) * 0.85), 28.0) * 0.55;

        float3 H = normalize(V + normalize(float3(0.40, 0.90, 0.30)));
        float  hot   = pow(saturate(dot(N, H)), 220.0);
        float  shine = pow(saturate(dot(N, H)),  55.0);

        float3 col = env * tint * fresMetal
                   + streak * tint * 1.10
                   + hot * 1.80
                   + shine * silver * 0.35;

        result.rgb = col;
        result.a   = base_color.a;
    }
    else
    {
        float3 R = reflect(-V, N);

        float phase = (1.0 - facing) * 3.20 + R.y * 0.55 + R.x * 0.30;

        float3 spectrum = float3(
            sin(phase * 6.2832 + 0.000) * 0.5 + 0.5,
            sin(phase * 6.2832 + 2.094) * 0.5 + 0.5,
            sin(phase * 6.2832 + 4.188) * 0.5 + 0.5);
        spectrum = pow(spectrum, float3(1.3, 1.3, 1.3)) * 1.35;

        float secondary = sin(phase * 12.5 + N.y * 2.2) * 0.5 + 0.5;
        spectrum *= 0.55 + 0.55 * secondary;

        float3 H = normalize(V + normalize(float3(0.3, 0.9, 0.2)));
        float spark = pow(saturate(dot(N, H)), 140.0) * 1.80
                    + pow(saturate(dot(N, H)),  40.0) * 0.25;

        float rim = pow(saturate(1.0 - facing), 1.7);

        float3 col = spectrum + base_color.rgb * 0.22;
        col += spectrum * rim * 0.70;
        col += spark;

        result.rgb = col;
        result.a   = base_color.a;
    }

    result.rgb *= brightness;
    return result;
}
)HLSL";

	struct CBData
	{
		float view[16];
		float world[16];
		float camera[3];
		float _pad_camera;
		float base_color[4];
		float fresnel_color[4];
		int mode;
		float fresnel_power;
		float brightness;
		float _pad_tail;
	};
	static_assert(sizeof(CBData) % 16 == 0, "CBData must be 16-byte aligned");

	struct Vertex
	{
		float pos[3];
		float normal[3];
	};

	ID3D11Device* g_device = nullptr;
	ID3D11DeviceContext* g_context = nullptr;
	ID3D11VertexShader* g_vs = nullptr;
	ID3D11PixelShader* g_ps = nullptr;
	ID3D11InputLayout* g_layout = nullptr;
	ID3D11Buffer* g_cb = nullptr;

	ID3D11BlendState* g_blendAlpha = nullptr;
	ID3D11BlendState* g_blendAdd = nullptr;
	ID3D11BlendState* g_blendNoColor = nullptr;
	ID3D11DepthStencilState* g_dsPrepass = nullptr;
	ID3D11DepthStencilState* g_dsColor = nullptr;
	ID3D11RasterizerState* g_rasterState = nullptr;

	ID3D11Texture2D* g_depthTex = nullptr;
	ID3D11DepthStencilView* g_dsv = nullptr;

	ID3D11Buffer* g_cubeVB = nullptr;
	ID3D11Buffer* g_cubeIB = nullptr;
	UINT g_cubeIndexCount = 0;

	unsigned g_width = 0;
	unsigned g_height = 0;
	std::atomic<bool> g_ready{ false };

	bool CreateShaders()
	{
		ID3DBlob* vsb = nullptr;
		ID3DBlob* psb = nullptr;
		ID3DBlob* err = nullptr;

		HRESULT hr = D3DCompile(kShaderHLSL, sizeof(kShaderHLSL) - 1, "chams", nullptr, nullptr,
			"vs_main", "vs_4_0", 0, 0, &vsb, &err);
		if (FAILED(hr)) { if (err) err->Release(); return false; }

		hr = D3DCompile(kShaderHLSL, sizeof(kShaderHLSL) - 1, "chams", nullptr, nullptr,
			"ps_main", "ps_4_0", 0, 0, &psb, &err);
		if (FAILED(hr)) { if (err) err->Release(); vsb->Release(); return false; }

		hr = g_device->CreateVertexShader(vsb->GetBufferPointer(), vsb->GetBufferSize(), nullptr, &g_vs);
		if (FAILED(hr)) { vsb->Release(); psb->Release(); return false; }

		hr = g_device->CreatePixelShader(psb->GetBufferPointer(), psb->GetBufferSize(), nullptr, &g_ps);
		if (FAILED(hr)) { vsb->Release(); psb->Release(); return false; }

		const D3D11_INPUT_ELEMENT_DESC layout[] = {
			{ "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0,  0, D3D11_INPUT_PER_VERTEX_DATA, 0 },
			{ "NORMAL",   0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12, D3D11_INPUT_PER_VERTEX_DATA, 0 },
		};
		hr = g_device->CreateInputLayout(layout, 2,
			vsb->GetBufferPointer(), vsb->GetBufferSize(), &g_layout);
		vsb->Release();
		psb->Release();
		return SUCCEEDED(hr);
	}

	bool CreateStates()
	{
		D3D11_BUFFER_DESC cbd{};
		cbd.Usage = D3D11_USAGE_DYNAMIC;
		cbd.ByteWidth = sizeof(CBData);
		cbd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
		cbd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
		if (FAILED(g_device->CreateBuffer(&cbd, nullptr, &g_cb))) return false;

		D3D11_BLEND_DESC ba{};
		ba.RenderTarget[0].BlendEnable = TRUE;
		ba.RenderTarget[0].SrcBlend = D3D11_BLEND_SRC_ALPHA;
		ba.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
		ba.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
		ba.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
		ba.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
		ba.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
		ba.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
		if (FAILED(g_device->CreateBlendState(&ba, &g_blendAlpha))) return false;

		D3D11_BLEND_DESC bd{};
		bd.RenderTarget[0].BlendEnable = TRUE;
		bd.RenderTarget[0].SrcBlend = D3D11_BLEND_SRC_ALPHA;
		bd.RenderTarget[0].DestBlend = D3D11_BLEND_ONE;
		bd.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
		bd.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
		bd.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ONE;
		bd.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
		bd.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
		if (FAILED(g_device->CreateBlendState(&bd, &g_blendAdd))) return false;

		D3D11_BLEND_DESC bz{};
		bz.RenderTarget[0].BlendEnable = FALSE;
		bz.RenderTarget[0].RenderTargetWriteMask = 0;
		if (FAILED(g_device->CreateBlendState(&bz, &g_blendNoColor))) return false;

		D3D11_DEPTH_STENCIL_DESC dsp{};
		dsp.DepthEnable = TRUE;
		dsp.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
		dsp.DepthFunc = D3D11_COMPARISON_LESS;
		if (FAILED(g_device->CreateDepthStencilState(&dsp, &g_dsPrepass))) return false;

		D3D11_DEPTH_STENCIL_DESC dsc{};
		dsc.DepthEnable = TRUE;
		dsc.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
		dsc.DepthFunc = D3D11_COMPARISON_LESS_EQUAL;
		if (FAILED(g_device->CreateDepthStencilState(&dsc, &g_dsColor))) return false;

		D3D11_RASTERIZER_DESC rd{};
		rd.FillMode = D3D11_FILL_SOLID;
		rd.CullMode = D3D11_CULL_NONE;
		rd.DepthClipEnable = TRUE;
		if (FAILED(g_device->CreateRasterizerState(&rd, &g_rasterState))) return false;

		return true;
	}

	bool CreateUnitCube()
	{
		const Vertex verts[24] = {
			{ {  0.5f, -0.5f, -0.5f }, {  1.f, 0.f, 0.f } },
			{ {  0.5f,  0.5f, -0.5f }, {  1.f, 0.f, 0.f } },
			{ {  0.5f,  0.5f,  0.5f }, {  1.f, 0.f, 0.f } },
			{ {  0.5f, -0.5f,  0.5f }, {  1.f, 0.f, 0.f } },

			{ { -0.5f, -0.5f, -0.5f }, { -1.f, 0.f, 0.f } },
			{ { -0.5f, -0.5f,  0.5f }, { -1.f, 0.f, 0.f } },
			{ { -0.5f,  0.5f,  0.5f }, { -1.f, 0.f, 0.f } },
			{ { -0.5f,  0.5f, -0.5f }, { -1.f, 0.f, 0.f } },

			{ { -0.5f,  0.5f, -0.5f }, { 0.f,  1.f, 0.f } },
			{ {  0.5f,  0.5f, -0.5f }, { 0.f,  1.f, 0.f } },
			{ {  0.5f,  0.5f,  0.5f }, { 0.f,  1.f, 0.f } },
			{ { -0.5f,  0.5f,  0.5f }, { 0.f,  1.f, 0.f } },

			{ { -0.5f, -0.5f,  0.5f }, { 0.f, -1.f, 0.f } },
			{ {  0.5f, -0.5f,  0.5f }, { 0.f, -1.f, 0.f } },
			{ {  0.5f, -0.5f, -0.5f }, { 0.f, -1.f, 0.f } },
			{ { -0.5f, -0.5f, -0.5f }, { 0.f, -1.f, 0.f } },

			{ {  0.5f, -0.5f,  0.5f }, { 0.f, 0.f,  1.f } },
			{ { -0.5f, -0.5f,  0.5f }, { 0.f, 0.f,  1.f } },
			{ { -0.5f,  0.5f,  0.5f }, { 0.f, 0.f,  1.f } },
			{ {  0.5f,  0.5f,  0.5f }, { 0.f, 0.f,  1.f } },

			{ { -0.5f, -0.5f, -0.5f }, { 0.f, 0.f, -1.f } },
			{ { -0.5f,  0.5f, -0.5f }, { 0.f, 0.f, -1.f } },
			{ {  0.5f,  0.5f, -0.5f }, { 0.f, 0.f, -1.f } },
			{ {  0.5f, -0.5f, -0.5f }, { 0.f, 0.f, -1.f } },
		};

		const uint32_t idx[36] = {
			0,  1,  2,   0,  2,  3,
			4,  5,  6,   4,  6,  7,
			8,  9, 10,   8, 10, 11,
		   12, 13, 14,  12, 14, 15,
		   16, 17, 18,  16, 18, 19,
		   20, 21, 22,  20, 22, 23,
		};

		D3D11_BUFFER_DESC vbd{};
		vbd.Usage = D3D11_USAGE_IMMUTABLE;
		vbd.ByteWidth = sizeof(verts);
		vbd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
		D3D11_SUBRESOURCE_DATA vinit{ verts };
		if (FAILED(g_device->CreateBuffer(&vbd, &vinit, &g_cubeVB))) return false;

		D3D11_BUFFER_DESC ibd{};
		ibd.Usage = D3D11_USAGE_IMMUTABLE;
		ibd.ByteWidth = sizeof(idx);
		ibd.BindFlags = D3D11_BIND_INDEX_BUFFER;
		D3D11_SUBRESOURCE_DATA iinit{ idx };
		if (FAILED(g_device->CreateBuffer(&ibd, &iinit, &g_cubeIB))) return false;

		g_cubeIndexCount = 36;
		return true;
	}

	bool CreateDepthBuffer(unsigned w, unsigned h)
	{
		if (g_dsv) { g_dsv->Release(); g_dsv = nullptr; }
		if (g_depthTex) { g_depthTex->Release(); g_depthTex = nullptr; }

		D3D11_TEXTURE2D_DESC td{};
		td.Width = w;
		td.Height = h;
		td.MipLevels = 1;
		td.ArraySize = 1;
		td.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
		td.SampleDesc.Count = 1;
		td.Usage = D3D11_USAGE_DEFAULT;
		td.BindFlags = D3D11_BIND_DEPTH_STENCIL;
		if (FAILED(g_device->CreateTexture2D(&td, nullptr, &g_depthTex))) return false;
		if (FAILED(g_device->CreateDepthStencilView(g_depthTex, nullptr, &g_dsv))) return false;

		g_width = w;
		g_height = h;
		return true;
	}

	bool IsBasePart(const std::string& cls)
	{
		return cls == "Part" || cls == "MeshPart" || cls == "WedgePart" ||
			cls == "CornerWedgePart" || cls == "TrussPart" || cls == "UnionOperation" ||
			cls == "NegateOperation" || cls == "Seat" || cls == "VehicleSeat" ||
			cls == "SpawnLocation";
	}

	bool IsContainer(const std::string& cls)
	{
		if (cls == "Tool" || cls == "Model" || cls == "Folder") return true;
		if (cls.size() >= 9 && cls.compare(cls.size() - 9, 9, "Accessory") == 0) return true;
		return false;
	}

	std::string ReadClassName(Memory& mem, uintptr_t instance)
	{
		uintptr_t descriptor = mem.Read<uintptr_t>(instance + Offsets::Instance::ClassDescriptor);
		if (!descriptor) return "";
		uintptr_t classPtr = mem.Read<uintptr_t>(descriptor + Offsets::Instance::ClassName);
		if (!classPtr) return "";
		return mem.ReadString(classPtr);
	}

	uintptr_t FindSpecialMesh(Memory& mem, uintptr_t parent)
	{
		auto children = mem.GetChildren(parent);
		for (uintptr_t child : children)
		{
			if (ReadClassName(mem, child) == "SpecialMesh")
				return child;
		}
		return 0;
	}

	void BuildWorldMatrix(const Vector3& pos, const float rot[9], const Vector3& size, float out[16])
	{
		out[0]  = rot[0] * size.x; out[1]  = rot[1] * size.y; out[2]  = rot[2] * size.z; out[3]  = pos.x;
		out[4]  = rot[3] * size.x; out[5]  = rot[4] * size.y; out[6]  = rot[5] * size.z; out[7]  = pos.y;
		out[8]  = rot[6] * size.x; out[9]  = rot[7] * size.y; out[10] = rot[8] * size.z; out[11] = pos.z;
		out[12] = 0.f;             out[13] = 0.f;             out[14] = 0.f;             out[15] = 1.f;
	}

	bool WorldToScreen(const float view[16], float w, float h, float wx, float wy, float wz, ImVec2& out)
	{
		const float cx = view[0]  * wx + view[1]  * wy + view[2]  * wz + view[3];
		const float cy = view[4]  * wx + view[5]  * wy + view[6]  * wz + view[7];
		const float cw = view[12] * wx + view[13] * wy + view[14] * wz + view[15];
		if (cw < 0.1f) return false;
		const float ndcX = cx / cw;
		const float ndcY = cy / cw;
		out.x = (w * 0.5f) * ndcX + (w * 0.5f);
		out.y = (h * 0.5f) - (h * 0.5f) * ndcY;
		return true;
	}

	Vector3 RotateLocal(const float rot[9], const Vector3& v)
	{
		return {
			rot[0] * v.x + rot[1] * v.y + rot[2] * v.z,
			rot[3] * v.x + rot[4] * v.y + rot[5] * v.z,
			rot[6] * v.x + rot[7] * v.y + rot[8] * v.z,
		};
	}

	std::vector<ImVec2> ConvexHull(std::vector<ImVec2>& pts)
	{
		std::sort(pts.begin(), pts.end(), [](const ImVec2& a, const ImVec2& b) {
			return a.x < b.x || (a.x == b.x && a.y < b.y);
		});

		std::vector<ImVec2> hull;
		hull.reserve(pts.size());

		for (ImVec2& p : pts)
		{
			while (hull.size() >= 2)
			{
				ImVec2& O = hull[hull.size() - 2];
				ImVec2& A = hull[hull.size() - 1];
				float cross = (A.x - O.x) * (p.y - O.y) - (A.y - O.y) * (p.x - O.x);
				if (cross > 0) break;
				hull.pop_back();
			}
			hull.push_back(p);
		}

		const size_t lowerSize = hull.size() + 1;
		for (int i = (int)pts.size() - 2; i >= 0; --i)
		{
			ImVec2& p = pts[i];
			while (hull.size() >= lowerSize)
			{
				ImVec2& O = hull[hull.size() - 2];
				ImVec2& A = hull[hull.size() - 1];
				float cross = (A.x - O.x) * (p.y - O.y) - (A.y - O.y) * (p.x - O.x);
				if (cross > 0) break;
				hull.pop_back();
			}
			hull.push_back(p);
		}

		if (!hull.empty()) hull.pop_back();
		return hull;
	}
}

namespace Chams
{
	Settings& GetSettings()
	{
		static Settings settings;
		return settings;
	}

	bool Init(ID3D11Device* device, ID3D11DeviceContext* context)
	{
		g_device = device;
		g_context = context;
		if (!CreateShaders()) return false;
		if (!CreateStates()) return false;
		if (!CreateUnitCube()) return false;
		g_ready.store(true);
		return true;
	}

	void Shutdown()
	{
		g_ready.store(false);
		if (g_cubeIB) { g_cubeIB->Release(); g_cubeIB = nullptr; }
		if (g_cubeVB) { g_cubeVB->Release(); g_cubeVB = nullptr; }
		if (g_dsv) { g_dsv->Release(); g_dsv = nullptr; }
		if (g_depthTex) { g_depthTex->Release(); g_depthTex = nullptr; }
		if (g_rasterState) { g_rasterState->Release(); g_rasterState = nullptr; }
		if (g_dsColor) { g_dsColor->Release(); g_dsColor = nullptr; }
		if (g_dsPrepass) { g_dsPrepass->Release(); g_dsPrepass = nullptr; }
		if (g_blendNoColor) { g_blendNoColor->Release(); g_blendNoColor = nullptr; }
		if (g_blendAdd) { g_blendAdd->Release(); g_blendAdd = nullptr; }
		if (g_blendAlpha) { g_blendAlpha->Release(); g_blendAlpha = nullptr; }
		if (g_cb) { g_cb->Release(); g_cb = nullptr; }
		if (g_layout) { g_layout->Release(); g_layout = nullptr; }
		if (g_ps) { g_ps->Release(); g_ps = nullptr; }
		if (g_vs) { g_vs->Release(); g_vs = nullptr; }
		g_device = nullptr;
		g_context = nullptr;
	}

	void Resize(unsigned w, unsigned h)
	{
		if (!g_device || w == 0 || h == 0) return;
		CreateDepthBuffer(w, h);
	}

	void RenderConvex2D()
	{
		auto& settings = GetSettings();
		auto& mem = Memory::Get();
		if (!mem.base) return;

		uintptr_t visualEngine = mem.Read<uintptr_t>(mem.base + Offsets::VisualEngine::Pointer);
		if (!visualEngine) return;

		float viewMatrix[16] = { 0 };
		if (!mem.ReadMemory(visualEngine + Offsets::VisualEngine::ViewMatrix, viewMatrix, sizeof(viewMatrix)))
			return;

		float dims[2] = { 0.f, 0.f };
		if (!mem.ReadMemory(visualEngine + Offsets::VisualEngine::Dimensions, dims, sizeof(dims)))
			return;
		if (dims[0] <= 0.f || dims[1] <= 0.f) return;

		const float screenW = dims[0];
		const float screenH = dims[1];

		float offsetX = 0.f, offsetY = 0.f;
		HWND robloxWnd = Overlay::GetTargetWindow();
		HWND overlayWnd = Overlay::GetOverlayWindow();
		if (robloxWnd && IsWindow(robloxWnd) && overlayWnd && IsWindow(overlayWnd))
		{
			POINT clientOrigin = { 0, 0 };
			ClientToScreen(robloxWnd, &clientOrigin);
			POINT overlayOrigin = { 0, 0 };
			ClientToScreen(overlayWnd, &overlayOrigin);
			offsetX = (float)(clientOrigin.x - overlayOrigin.x);
			offsetY = (float)(clientOrigin.y - overlayOrigin.y);
		}

		uintptr_t fakeDataModel = mem.Read<uintptr_t>(mem.base + Offsets::FakeDataModel::Pointer);
		if (!fakeDataModel) return;
		uintptr_t dataModel = mem.Read<uintptr_t>(fakeDataModel + Offsets::FakeDataModel::RealDataModel);
		if (!dataModel) return;

		uintptr_t playersService = mem.FindChild(dataModel, "Players");
		if (!playersService) return;

		uintptr_t localPlayer = mem.Read<uintptr_t>(playersService + Offsets::Player::LocalPlayer);
		uintptr_t localTeam = localPlayer ? mem.Read<uintptr_t>(localPlayer + Offsets::Player::Team) : 0;

		const ImU32 fillCol = ImGui::ColorConvertFloat4ToU32(
			ImVec4(settings.color[0], settings.color[1], settings.color[2], settings.color[3]));
		const ImU32 outlineCol = ImGui::ColorConvertFloat4ToU32(
			ImVec4(settings.convexOutlineColor[0], settings.convexOutlineColor[1],
				settings.convexOutlineColor[2], settings.convexOutlineColor[3]));

		ImDrawList* dl = ImGui::GetBackgroundDrawList();

		static const Vector3 kUnitCorners[8] = {
			{ -1, -1, -1 }, {  1, -1, -1 }, { -1,  1, -1 }, {  1,  1, -1 },
			{ -1, -1,  1 }, {  1, -1,  1 }, { -1,  1,  1 }, {  1,  1,  1 },
		};

		auto playerList = mem.GetChildren(playersService);

		for (uintptr_t player : playerList)
		{
			if (player == localPlayer) continue;
			if (ReadClassName(mem, player) != "Player") continue;

			if (settings.teamCheck)
			{
				uintptr_t playerTeam = mem.Read<uintptr_t>(player + Offsets::Player::Team);
				if (localTeam != 0 && playerTeam != 0 && playerTeam == localTeam) continue;
			}

			uintptr_t character = mem.Read<uintptr_t>(player + Offsets::Player::ModelInstance);
			if (!character) continue;

			Clipper2Lib::Paths64 allHulls;

			std::function<void(uintptr_t, int)> walk;
			walk = [&](uintptr_t parent, int depth) {
				if (depth > 2) return;
				auto children = mem.GetChildren(parent);
				for (uintptr_t child : children)
				{
					std::string cls = ReadClassName(mem, child);
					if (IsBasePart(cls))
					{
						uintptr_t primitive = mem.Read<uintptr_t>(child + Offsets::BasePart::Primitive);
						if (!primitive) continue;

						Vector3 pos{}, size{};
						float rot[9] = { 0 };
						if (!mem.ReadMemory(primitive + Offsets::Primitive::Position, &pos, sizeof(pos))) continue;
						if (!mem.ReadMemory(primitive + Offsets::Primitive::Size, &size, sizeof(size))) continue;
						if (!mem.ReadMemory(primitive + Offsets::Primitive::Rotation, rot, sizeof(rot))) continue;

						float transparency = 0.f;
						mem.ReadMemory(child + Offsets::BasePart::Transparency, &transparency, sizeof(transparency));
						if (transparency >= 0.99f) continue;

						std::vector<ImVec2> projected;
						projected.reserve(8);
						for (const Vector3& c : kUnitCorners)
						{
							Vector3 local = { c.x * size.x * 0.5f, c.y * size.y * 0.5f, c.z * size.z * 0.5f };
							Vector3 rotated = RotateLocal(rot, local);
							const float wx = pos.x + rotated.x;
							const float wy = pos.y + rotated.y;
							const float wz = pos.z + rotated.z;
							ImVec2 sp;
							if (WorldToScreen(viewMatrix, screenW, screenH, wx, wy, wz, sp))
							{
								projected.push_back(ImVec2(sp.x + offsetX, sp.y + offsetY));
							}
						}

						if (projected.size() < 3) continue;
						std::vector<ImVec2> hull = ConvexHull(projected);
						if (hull.size() < 3) continue;

						Clipper2Lib::Path64 path;
						path.reserve(hull.size());
						for (ImVec2& pt : hull)
						{
							path.push_back({
								static_cast<int64_t>(pt.x * 1000.0),
								static_cast<int64_t>(pt.y * 1000.0)
							});
						}
						allHulls.push_back(std::move(path));
					}
					else if (settings.accessories && IsContainer(cls))
					{
						walk(child, depth + 1);
					}
				}
			};

			walk(character, 0);

			if (allHulls.empty()) continue;

			Clipper2Lib::Paths64 unified = Clipper2Lib::Union(allHulls, Clipper2Lib::FillRule::NonZero);
			for (Clipper2Lib::Path64& sp : unified)
			{
				std::vector<ImVec2> poly;
				poly.reserve(sp.size());
				for (Clipper2Lib::Point64& pt : sp)
				{
					poly.emplace_back(
						static_cast<float>(pt.x) / 1000.f,
						static_cast<float>(pt.y) / 1000.f);
				}
				if (poly.size() < 3) continue;

				dl->AddConcavePolyFilled(poly.data(), (int)poly.size(), fillCol);
				if (settings.convexOutline)
				{
					dl->AddPolyline(poly.data(), (int)poly.size(), outlineCol, ImDrawFlags_Closed, 1.f);
				}
			}
		}
	}

	void RenderImGui()
	{
		auto& settings = GetSettings();
		if (!settings.enabled) return;
		if (settings.kind != 1) return;
		RenderConvex2D();
	}

	void Render(ID3D11RenderTargetView* rtv)
	{
		auto& settings = GetSettings();
		if (!settings.enabled) return;
		if (settings.kind == 1) return;
		if (!g_ready.load() || !g_device || !g_dsv) return;

		auto& mem = Memory::Get();
		if (!mem.base) return;

		uintptr_t visualEngine = mem.Read<uintptr_t>(mem.base + Offsets::VisualEngine::Pointer);
		if (!visualEngine) return;

		float viewMatrix[16] = { 0 };
		if (!mem.ReadMemory(visualEngine + Offsets::VisualEngine::ViewMatrix, viewMatrix, sizeof(viewMatrix)))
			return;

		uintptr_t fakeDataModel = mem.Read<uintptr_t>(mem.base + Offsets::FakeDataModel::Pointer);
		if (!fakeDataModel) return;
		uintptr_t dataModel = mem.Read<uintptr_t>(fakeDataModel + Offsets::FakeDataModel::RealDataModel);
		if (!dataModel) return;
		uintptr_t workspace = mem.Read<uintptr_t>(dataModel + Offsets::DataModel::Workspace);
		if (!workspace) return;
		uintptr_t camera = mem.Read<uintptr_t>(workspace + Offsets::Workspace::CurrentCamera);
		if (!camera) return;

		Vector3 camPos{};
		if (!mem.ReadMemory(camera + Offsets::Camera::Position, &camPos, sizeof(camPos)))
			return;

		uintptr_t playersService = mem.FindChild(dataModel, "Players");
		if (!playersService) return;

		uintptr_t localPlayer = mem.Read<uintptr_t>(playersService + Offsets::Player::LocalPlayer);
		uintptr_t localTeam = localPlayer ? mem.Read<uintptr_t>(localPlayer + Offsets::Player::Team) : 0;

		struct PartDraw
		{
			float world[16];
			ID3D11Buffer* vb;
			ID3D11Buffer* ib;
			UINT indexCount;
			float dist2;
		};
		std::vector<PartDraw> draws;
		draws.reserve(64);

		auto playerList = mem.GetChildren(playersService);

		for (uintptr_t player : playerList)
		{
			if (player == localPlayer) continue;

			std::string playerClass = ReadClassName(mem, player);
			if (playerClass != "Player") continue;

			if (settings.teamCheck)
			{
				uintptr_t playerTeam = mem.Read<uintptr_t>(player + Offsets::Player::Team);
				if (localTeam != 0 && playerTeam != 0 && playerTeam == localTeam)
					continue;
			}

			uintptr_t character = mem.Read<uintptr_t>(player + Offsets::Player::ModelInstance);
			if (!character) continue;

			std::function<void(uintptr_t, int)> walk;
			walk = [&](uintptr_t parent, int depth) {
				if (depth > 2) return;
				auto children = mem.GetChildren(parent);
				for (uintptr_t child : children)
				{
					std::string cls = ReadClassName(mem, child);

					if (IsBasePart(cls))
					{
						uintptr_t primitive = mem.Read<uintptr_t>(child + Offsets::BasePart::Primitive);
						if (!primitive) continue;

						Vector3 pos{}, size{};
						float rot[9] = { 0 };
						if (!mem.ReadMemory(primitive + Offsets::Primitive::Position, &pos, sizeof(pos))) continue;
						if (!mem.ReadMemory(primitive + Offsets::Primitive::Size, &size, sizeof(size))) continue;
						if (!mem.ReadMemory(primitive + Offsets::Primitive::Rotation, rot, sizeof(rot))) continue;

						float transparency = 0.f;
						mem.ReadMemory(child + Offsets::BasePart::Transparency, &transparency, sizeof(transparency));
						if (transparency >= 0.99f) continue;

						const float dx = pos.x - camPos.x;
						const float dy = pos.y - camPos.y;
						const float dz = pos.z - camPos.z;
						const float dist2 = dx * dx + dy * dy + dz * dz;
						if (dist2 > 5000.f * 5000.f) continue;

						PartDraw d{};
						BuildWorldMatrix(pos, rot, size, d.world);

						MeshCache::GpuMesh* mesh = nullptr;
						bool isSpecialMesh = false;
						float specialMeshScale[3] = { 1.f, 1.f, 1.f };
						std::string meshId;

						if (cls == "MeshPart")
						{
							meshId = mem.ReadString(child + Offsets::MeshPart::MeshId);
						}
						else if (cls == "UnionOperation" || cls == "NegateOperation")
						{
							meshId = mem.ReadString(child + Offsets::UnionOperation::AssetId);
						}
						else
						{
							uintptr_t specialMesh = FindSpecialMesh(mem, child);
							if (specialMesh)
							{
								meshId = mem.ReadString(specialMesh + Offsets::SpecialMesh::MeshId);
								if (!meshId.empty())
								{
									isSpecialMesh = true;
									mem.ReadMemory(specialMesh + Offsets::SpecialMesh::Scale,
										specialMeshScale, sizeof(specialMeshScale));
								}
							}
						}

						if (!meshId.empty())
						{
							mesh = MeshCache::TryGet(meshId, g_device);
							if (mesh && mesh->meshSize[0] > 0.f)
							{
								Vector3 scaled;
								if (isSpecialMesh)
								{
									scaled = { specialMeshScale[0], specialMeshScale[1], specialMeshScale[2] };
								}
								else
								{
									scaled = {
										size.x / mesh->meshSize[0],
										size.y / mesh->meshSize[1],
										size.z / mesh->meshSize[2],
									};
								}
								BuildWorldMatrix(pos, rot, scaled, d.world);
							}
						}

						if (mesh && mesh->vb && mesh->ib && mesh->indexCount > 0)
						{
							d.vb = mesh->vb;
							d.ib = mesh->ib;
							d.indexCount = mesh->indexCount;
						}
						else
						{
							d.vb = g_cubeVB;
							d.ib = g_cubeIB;
							d.indexCount = g_cubeIndexCount;
						}

						d.dist2 = dist2;
						draws.push_back(d);
					}
					else if (settings.accessories && IsContainer(cls))
					{
						walk(child, depth + 1);
					}
				}
			};

			walk(character, 0);
		}

		if (draws.empty()) return;

		std::sort(draws.begin(), draws.end(),
			[](const PartDraw& a, const PartDraw& b) { return a.dist2 < b.dist2; });

		g_context->OMSetRenderTargets(1, &rtv, g_dsv);
		g_context->ClearDepthStencilView(g_dsv, D3D11_CLEAR_DEPTH, 1.f, 0);

		float vpX = 0.f, vpY = 0.f;
		float vpW = (float)g_width;
		float vpH = (float)g_height;

		HWND robloxWnd = Overlay::GetTargetWindow();
		HWND overlayWnd = Overlay::GetOverlayWindow();
		if (robloxWnd && IsWindow(robloxWnd) && overlayWnd && IsWindow(overlayWnd))
		{
			RECT clientRect{};
			GetClientRect(robloxWnd, &clientRect);

			POINT clientOrigin = { 0, 0 };
			ClientToScreen(robloxWnd, &clientOrigin);

			POINT overlayOrigin = { 0, 0 };
			ClientToScreen(overlayWnd, &overlayOrigin);

			vpX = (float)(clientOrigin.x - overlayOrigin.x);
			vpY = (float)(clientOrigin.y - overlayOrigin.y);
			vpW = (float)(clientRect.right - clientRect.left);
			vpH = (float)(clientRect.bottom - clientRect.top);

			if (vpW <= 0.f) vpW = (float)g_width;
			if (vpH <= 0.f) vpH = (float)g_height;
		}

		D3D11_VIEWPORT vp{ vpX, vpY, vpW, vpH, 0.f, 1.f };
		g_context->RSSetViewports(1, &vp);
		g_context->RSSetState(g_rasterState);

		const FLOAT bf[4]{ 0, 0, 0, 0 };

		g_context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
		g_context->IASetInputLayout(g_layout);
		g_context->VSSetShader(g_vs, nullptr, 0);
		g_context->PSSetShader(g_ps, nullptr, 0);
		g_context->VSSetConstantBuffers(0, 1, &g_cb);
		g_context->PSSetConstantBuffers(0, 1, &g_cb);

		const UINT stride = sizeof(Vertex);
		const UINT offset = 0;

		CBData cb{};
		std::memcpy(cb.view, viewMatrix, sizeof(viewMatrix));
		cb.camera[0] = camPos.x;
		cb.camera[1] = camPos.y;
		cb.camera[2] = camPos.z;
		int m = settings.mode;
		if (m < 0 || m > 4) m = 1;
		cb.mode = m;
		cb.fresnel_power = settings.fresnelPower;
		cb.brightness = settings.brightness;
		std::memcpy(cb.base_color, settings.color, sizeof(cb.base_color));
		std::memcpy(cb.fresnel_color, settings.fresnelColor, sizeof(cb.fresnel_color));

		auto issueDraws = [&]() {
			ID3D11Buffer* prevVB = nullptr;
			ID3D11Buffer* prevIB = nullptr;
			for (auto& d : draws)
			{
				if (d.vb != prevVB)
				{
					g_context->IASetVertexBuffers(0, 1, &d.vb, &stride, &offset);
					prevVB = d.vb;
				}
				if (d.ib != prevIB)
				{
					g_context->IASetIndexBuffer(d.ib, DXGI_FORMAT_R32_UINT, 0);
					prevIB = d.ib;
				}
				std::memcpy(cb.world, d.world, sizeof(d.world));
				D3D11_MAPPED_SUBRESOURCE ms;
				if (FAILED(g_context->Map(g_cb, 0, D3D11_MAP_WRITE_DISCARD, 0, &ms))) continue;
				std::memcpy(ms.pData, &cb, sizeof(cb));
				g_context->Unmap(g_cb, 0);
				g_context->DrawIndexed(d.indexCount, 0, 0);
			}
		};

		g_context->OMSetBlendState(g_blendNoColor, bf, 0xFFFFFFFF);
		g_context->OMSetDepthStencilState(g_dsPrepass, 0);
		issueDraws();

		g_context->OMSetBlendState(settings.additive ? g_blendAdd : g_blendAlpha, bf, 0xFFFFFFFF);
		g_context->OMSetDepthStencilState(g_dsColor, 0);
		issueDraws();
	}
}
