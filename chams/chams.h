#pragma once
#include <d3d11.h>

namespace Chams
{
	struct Settings
	{
		bool enabled = false;
		int kind = 0;
		int mode = 1;
		float color[4] = { 1.f, 0.f, 1.f, 0.5f };
		float fresnelColor[4] = { 1.f, 1.f, 1.f, 1.f };
		float fresnelPower = 2.f;
		float brightness = 1.f;
		bool additive = false;
		bool teamCheck = false;
		bool accessories = false;
		bool convexOutline = true;
		float convexOutlineColor[4] = { 0.f, 0.f, 0.f, 1.f };
	};

	Settings& GetSettings();

	bool Init(ID3D11Device* device, ID3D11DeviceContext* context);
	void Shutdown();
	void Resize(unsigned w, unsigned h);
	void RenderImGui();
	void Render(ID3D11RenderTargetView* rtv);
}
