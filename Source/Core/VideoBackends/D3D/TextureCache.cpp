// Copyright 2013 Dolphin Emulator Project
// Licensed under GPLv2
// Refer to the license.txt file included.

#include "Core/HW/Memmap.h"
#include "VideoBackends/D3D/D3DBase.h"
#include "VideoBackends/D3D/D3DShader.h"
#include "VideoBackends/D3D/D3DState.h"
#include "VideoBackends/D3D/D3DUtil.h"
#include "VideoBackends/D3D/FramebufferManager.h"
#include "VideoBackends/D3D/GeometryShaderCache.h"
#include "VideoBackends/D3D/PixelShaderCache.h"
#include "VideoBackends/D3D/PSTextureEncoder.h"
#include "VideoBackends/D3D/TextureCache.h"
#include "VideoBackends/D3D/TextureEncoder.h"
#include "VideoBackends/D3D/VertexShaderCache.h"
#include "VideoCommon/ImageWrite.h"
#include "VideoCommon/LookUpTables.h"
#include "VideoCommon/RenderBase.h"
#include "VideoCommon/VideoConfig.h"
#include "VideoCommon/TextureCacheBase.h"

namespace DX11
{

static TextureEncoder* g_encoder = nullptr;
const size_t MAX_COPY_BUFFERS = 32;
ID3D11Buffer* efbcopycbuf[MAX_COPY_BUFFERS] = { 0 };

TextureCache::TCacheEntry::~TCacheEntry()
{
	texture->Release();
}

void TextureCache::TCacheEntry::Bind(unsigned int stage)
{
	D3D::stateman->SetTexture(stage, texture->GetSRV());
}

bool TextureCache::TCacheEntry::Save(const std::string& filename, unsigned int level)
{
	// TODO: Somehow implement this (D3DX11 doesn't support dumping individual LODs)
	static bool warn_once = true;
	if (level && warn_once)
	{
		WARN_LOG(VIDEO, "Dumping individual LOD not supported by D3D11 backend!");
		warn_once = false;
		return false;
	}

	ID3D11Texture2D* pNewTexture = nullptr;
	ID3D11Texture2D* pSurface = texture->GetTex();
	D3D11_TEXTURE2D_DESC desc;
	pSurface->GetDesc(&desc);

	desc.BindFlags = 0;
	desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ | D3D11_CPU_ACCESS_WRITE;
	desc.Usage = D3D11_USAGE_STAGING;

	HRESULT hr = D3D::device->CreateTexture2D(&desc, nullptr, &pNewTexture);

	bool saved_png = false;

	if (SUCCEEDED(hr) && pNewTexture)
	{
		D3D::context->CopyResource(pNewTexture, pSurface);

		D3D11_MAPPED_SUBRESOURCE map;
		HRESULT hr = D3D::context->Map(pNewTexture, 0, D3D11_MAP_READ_WRITE, 0, &map);
		if (SUCCEEDED(hr))
		{
			saved_png = TextureToPng((u8*)map.pData, map.RowPitch, filename, desc.Width, desc.Height);
			D3D::context->Unmap(pNewTexture, 0);
		}
		SAFE_RELEASE(pNewTexture);
	}

	return saved_png;
}

void TextureCache::TCacheEntry::Load(unsigned int width, unsigned int height,
	unsigned int expanded_width, unsigned int level)
{
	D3D::ReplaceRGBATexture2D(texture->GetTex(), TextureCache::temp, width, height, expanded_width, level, usage);
}

TextureCache::TCacheEntryBase* TextureCache::CreateTexture(const TCacheEntryConfig& config)
{
	if (config.rendertarget)
	{
		return new TCacheEntry(config, D3DTexture2D::Create(config.width, config.height,
			(D3D11_BIND_FLAG)((int)D3D11_BIND_RENDER_TARGET | (int)D3D11_BIND_SHADER_RESOURCE),
			D3D11_USAGE_DEFAULT, DXGI_FORMAT_R8G8B8A8_UNORM, 1, config.layers));
	}
	else
	{
		D3D11_USAGE usage = D3D11_USAGE_DEFAULT;
		D3D11_CPU_ACCESS_FLAG cpu_access = (D3D11_CPU_ACCESS_FLAG)0;

		if (config.levels == 1)
		{
			usage = D3D11_USAGE_DYNAMIC;
			cpu_access = D3D11_CPU_ACCESS_WRITE;
		}

		const D3D11_TEXTURE2D_DESC texdesc = CD3D11_TEXTURE2D_DESC(DXGI_FORMAT_R8G8B8A8_UNORM,
			config.width, config.height, 1, config.levels, D3D11_BIND_SHADER_RESOURCE, usage, cpu_access);

		ID3D11Texture2D *pTexture;
		const HRESULT hr = D3D::device->CreateTexture2D(&texdesc, nullptr, &pTexture);
		CHECK(SUCCEEDED(hr), "Create texture of the TextureCache");

		TCacheEntry* const entry = new TCacheEntry(config, new D3DTexture2D(pTexture, D3D11_BIND_SHADER_RESOURCE));
		entry->usage = usage;

		// TODO: better debug names
		D3D::SetDebugObjectName((ID3D11DeviceChild*)entry->texture->GetTex(), "a texture of the TextureCache");
		D3D::SetDebugObjectName((ID3D11DeviceChild*)entry->texture->GetSRV(), "shader resource view of a texture of the TextureCache");

		SAFE_RELEASE(pTexture);

		return entry;
	}
}

void TextureCache::TCacheEntry::FromRenderTarget(u32 dstAddr, unsigned int dstFormat,
	PEControl::PixelFormat srcFormat, const EFBRectangle& srcRect,
	bool isIntensity, bool scaleByHalf, unsigned int cbufid,
	const float *colmat)
{
	g_renderer->ResetAPIState();

	// stretch picture with increased internal resolution
	const D3D11_VIEWPORT vp = CD3D11_VIEWPORT(0.f, 0.f, (float)config.width, (float)config.height);
	D3D::context->RSSetViewports(1, &vp);

	// set transformation
	if (nullptr == efbcopycbuf[cbufid])
	{
		const D3D11_BUFFER_DESC cbdesc = CD3D11_BUFFER_DESC(28 * sizeof(float), D3D11_BIND_CONSTANT_BUFFER, D3D11_USAGE_DEFAULT);
		D3D11_SUBRESOURCE_DATA data;
		data.pSysMem = colmat;
		HRESULT hr = D3D::device->CreateBuffer(&cbdesc, &data, &efbcopycbuf[cbufid]);
		CHECK(SUCCEEDED(hr), "Create efb copy constant buffer %d", cbufid);
		D3D::SetDebugObjectName((ID3D11DeviceChild*)efbcopycbuf[cbufid], "a constant buffer used in TextureCache::CopyRenderTargetToTexture");
	}
	D3D::stateman->SetPixelConstants(efbcopycbuf[cbufid]);

	const TargetRectangle targetSource = g_renderer->ConvertEFBRectangle(srcRect);
	// TODO: try targetSource.asRECT();
	const D3D11_RECT sourcerect = CD3D11_RECT(targetSource.left, targetSource.top, targetSource.right, targetSource.bottom);

	// Use linear filtering if (bScaleByHalf), use point filtering otherwise
	if (scaleByHalf)
		D3D::SetLinearCopySampler();
	else
		D3D::SetPointCopySampler();

	// Make sure we don't draw with the texture set as both a source and target.
	// (This can happen because we don't unbind textures when we free them.)
	D3D::stateman->UnsetTexture(texture->GetSRV());

	D3D::context->OMSetRenderTargets(1, &texture->GetRTV(), nullptr);

	// Create texture copy
	D3D::drawShadedTexQuad(
		(srcFormat == PEControl::Z24) ? FramebufferManager::GetEFBDepthTexture()->GetSRV() : FramebufferManager::GetEFBColorTexture()->GetSRV(),
		&sourcerect, Renderer::GetTargetWidth(), Renderer::GetTargetHeight(),
		(srcFormat == PEControl::Z24) ? PixelShaderCache::GetDepthMatrixProgram(true) : PixelShaderCache::GetColorMatrixProgram(true),
		VertexShaderCache::GetSimpleVertexShader(), VertexShaderCache::GetSimpleInputLayout(), GeometryShaderCache::GetCopyGeometryShader());

	D3D::context->OMSetRenderTargets(1, &FramebufferManager::GetEFBColorTexture()->GetRTV(), FramebufferManager::GetEFBDepthTexture()->GetDSV());

	g_renderer->RestoreAPIState();

	if (!g_ActiveConfig.bSkipEFBCopyToRam)
	{
		u8* dst = Memory::GetPointer(dstAddr);
		size_t encoded_size = g_encoder->Encode(dst, dstFormat, srcFormat, srcRect, isIntensity, scaleByHalf);

		u64 hash = GetHash64(dst, (int)encoded_size, g_ActiveConfig.iSafeTextureCache_ColorSamples);

		size_in_bytes = (u32)encoded_size;

		TextureCache::MakeRangeDynamic(dstAddr, (u32)encoded_size);

		this->hash = hash;
	}

	if (bpmem.copyMipMapStrideChannels = 256)
	{
		TCacheEntryBase* t_entry;
		TextureCache::xyz(dstAddr, (u32)64 * 1024, &t_entry);

		if (t_entry->native_width == 1024)
		{
			u32 y = (dstAddr - t_entry->addr) / 0x10000;
			u32 x = ((dstAddr - t_entry->addr) % 0x10000) / 0x100;

			//ERROR_LOG(VIDEO, "dstAddr: 0x%08x/0x%08x, x/y: %d/%d", dstAddr, t_entry->addr, x, y);
			u8* dst = Memory::GetPointer(dstAddr);

			t_entry->hash = 0;

			if (((TCacheEntry *)t_entry)->usage == D3D11_USAGE_DYNAMIC || ((TCacheEntry *)t_entry)->usage == D3D11_USAGE_STAGING)
			{
				ERROR_LOG(VIDEO, "true: %d", ((TCacheEntry *)t_entry)->usage);
			}
			else
			{
				ERROR_LOG(VIDEO, "false: %d", ((TCacheEntry *)t_entry)->usage);

				D3D11_BOX dest_region = CD3D11_BOX(x * 32, y * 32, 0, x * 32 + 32, y * 32 + 32, 1);
				for (u32 i = 0; i < t_entry->native_levels; ++i)
				{
					D3D::context->UpdateSubresource(((TCacheEntry *)t_entry)->texture->GetTex(), i/*level*/, &dest_region, texture->GetTex()/*TextureCache::temp*/, 4 * 32/*expanded_width*/, 4 * 32 * 32/*expanded_width*height*/);
				}
			}
			/*
			01:04:009 TextureCache.cpp:209 E[Video]: dstAddr: 0x1036a460/0x10368560, x/y: 31/0
			01:04:009 TextureCache.cpp:209 E[Video]: dstAddr: 0x1037a360/0x10368560, x/y: 30/1
			01:04:009 TextureCache.cpp:209 E[Video]: dstAddr: 0x1036a360/0x10368560, x/y: 30/0
			01:04:009 TextureCache.cpp:209 E[Video]: dstAddr: 0x10379460/0x10368560, x/y: 15/1
			01:04:009 TextureCache.cpp:209 E[Video]: dstAddr: 0x10379560/0x10368560, x/y: 16/1
			01:04:010 TextureCache.cpp:209 E[Video]: dstAddr: 0x10379660/0x10368560, x/y: 17/1
			*/
		}
	}
}

const char palette_shader[] =
R"HLSL(
sampler samp0 : register(s0);
Texture2DArray Tex0 : register(t0);
Buffer<uint> Tex1 : register(t1);
uniform float Multiply;

uint Convert3To8(uint v)
{
	// Swizzle bits: 00000123 -> 12312312
	return (v << 5) | (v << 2) | (v >> 1);
}

uint Convert4To8(uint v)
{
	// Swizzle bits: 00001234 -> 12341234
	return (v << 4) | v;
}

uint Convert5To8(uint v)
{
	// Swizzle bits: 00012345 -> 12345123
	return (v << 3) | (v >> 2);
}

uint Convert6To8(uint v)
{
	// Swizzle bits: 00123456 -> 12345612
	return (v << 2) | (v >> 4);
}

float4 DecodePixel_RGB5A3(uint val)
{
	int r,g,b,a;
	if ((val&0x8000))
	{
		r=Convert5To8((val>>10) & 0x1f);
		g=Convert5To8((val>>5 ) & 0x1f);
		b=Convert5To8((val    ) & 0x1f);
		a=0xFF;
	}
	else
	{
		a=Convert3To8((val>>12) & 0x7);
		r=Convert4To8((val>>8 ) & 0xf);
		g=Convert4To8((val>>4 ) & 0xf);
		b=Convert4To8((val    ) & 0xf);
	}
	return float4(r, g, b, a) / 255;
}

float4 DecodePixel_RGB565(uint val)
{
	int r, g, b, a;
	r = Convert5To8((val >> 11) & 0x1f);
	g = Convert6To8((val >> 5) & 0x3f);
	b = Convert5To8((val) & 0x1f);
	a = 0xFF;
	return float4(r, g, b, a) / 255;
}

float4 DecodePixel_IA8(uint val)
{
	int i = val & 0xFF;
	int a = val >> 8;
	return float4(i, i, i, a) / 255;
}

void main(
	out float4 ocol0 : SV_Target,
	in float4 pos : SV_Position,
	in float3 uv0 : TEXCOORD0)
{
	uint src = round(Tex0.Sample(samp0,uv0) * Multiply).r;
	src = Tex1.Load(src);
	src = ((src << 8) & 0xFF00) | (src >> 8);
	ocol0 = DECODE(src);
}
)HLSL";

void TextureCache::ConvertTexture(TCacheEntryBase* entry, TCacheEntryBase* unconverted, void* palette, TlutFormat format)
{
	g_renderer->ResetAPIState();

	// stretch picture with increased internal resolution
	const D3D11_VIEWPORT vp = CD3D11_VIEWPORT(0.f, 0.f, (float)unconverted->config.width, (float)unconverted->config.height);
	D3D::context->RSSetViewports(1, &vp);

	D3D11_BOX box{ 0, 0, 0, 512, 1, 1 };
	D3D::context->UpdateSubresource(palette_buf, 0, &box, palette, 0, 0);

	D3D::stateman->SetTexture(1, palette_buf_srv);

	// TODO: Add support for C14X2 format.  (Different multiplier, more palette entries.)
	float params[4] = { unconverted->format == 0 ? 15.f : 255.f };
	D3D::context->UpdateSubresource(palette_uniform, 0, nullptr, &params, 0, 0);
	D3D::stateman->SetPixelConstants(palette_uniform);

	const D3D11_RECT sourcerect = CD3D11_RECT(0, 0, unconverted->config.width, unconverted->config.height);

	D3D::SetPointCopySampler();

	// Make sure we don't draw with the texture set as both a source and target.
	// (This can happen because we don't unbind textures when we free them.)
	D3D::stateman->UnsetTexture(static_cast<TCacheEntry*>(entry)->texture->GetSRV());

	D3D::context->OMSetRenderTargets(1, &static_cast<TCacheEntry*>(entry)->texture->GetRTV(), nullptr);

	// Create texture copy
	D3D::drawShadedTexQuad(
		static_cast<TCacheEntry*>(unconverted)->texture->GetSRV(),
		&sourcerect, unconverted->config.width, unconverted->config.height,
		palette_pixel_shader[format],
		VertexShaderCache::GetSimpleVertexShader(), VertexShaderCache::GetSimpleInputLayout(),
		GeometryShaderCache::GetCopyGeometryShader());

	D3D::context->OMSetRenderTargets(1, &FramebufferManager::GetEFBColorTexture()->GetRTV(), FramebufferManager::GetEFBDepthTexture()->GetDSV());

	g_renderer->RestoreAPIState();
}

ID3D11PixelShader *GetConvertShader(const char* Type)
{
	std::string shader = "#define DECODE DecodePixel_";
	shader.append(Type);
	shader.append("\n");
	shader.append(palette_shader);
	return D3D::CompileAndCreatePixelShader(shader);
}

TextureCache::TextureCache()
{
	// FIXME: Is it safe here?
	g_encoder = new PSTextureEncoder;
	g_encoder->Init();

	palette_buf = nullptr;
	palette_buf_srv = nullptr;
	palette_uniform = nullptr;
	palette_pixel_shader[GX_TL_IA8] = GetConvertShader("IA8");
	palette_pixel_shader[GX_TL_RGB565] = GetConvertShader("RGB565");
	palette_pixel_shader[GX_TL_RGB5A3] = GetConvertShader("RGB5A3");
	auto lutBd = CD3D11_BUFFER_DESC(sizeof(u16) * 256, D3D11_BIND_SHADER_RESOURCE);
	HRESULT hr = D3D::device->CreateBuffer(&lutBd, nullptr, &palette_buf);
	CHECK(SUCCEEDED(hr), "create palette decoder lut buffer");
	D3D::SetDebugObjectName(palette_buf, "texture decoder lut buffer");
	// TODO: C14X2 format.
	auto outlutUavDesc = CD3D11_SHADER_RESOURCE_VIEW_DESC(palette_buf, DXGI_FORMAT_R16_UINT, 0, 256, 0);
	hr = D3D::device->CreateShaderResourceView(palette_buf, &outlutUavDesc, &palette_buf_srv);
	CHECK(SUCCEEDED(hr), "create palette decoder lut srv");
	D3D::SetDebugObjectName(palette_buf_srv, "texture decoder lut srv");
	const D3D11_BUFFER_DESC cbdesc = CD3D11_BUFFER_DESC(16, D3D11_BIND_CONSTANT_BUFFER, D3D11_USAGE_DEFAULT);
	hr = D3D::device->CreateBuffer(&cbdesc, nullptr, &palette_uniform);
	CHECK(SUCCEEDED(hr), "Create palette decoder constant buffer");
	D3D::SetDebugObjectName((ID3D11DeviceChild*)palette_uniform, "a constant buffer used in TextureCache::CopyRenderTargetToTexture");
}

TextureCache::~TextureCache()
{
	for (unsigned int k = 0; k < MAX_COPY_BUFFERS; ++k)
		SAFE_RELEASE(efbcopycbuf[k]);

	g_encoder->Shutdown();
	delete g_encoder;
	g_encoder = nullptr;

	SAFE_RELEASE(palette_buf);
	SAFE_RELEASE(palette_buf_srv);
	SAFE_RELEASE(palette_uniform);
	for (ID3D11PixelShader*& shader : palette_pixel_shader)
		SAFE_RELEASE(shader);
}

}
