/******************************************************************************
 * The MIT License (MIT)
 *
 * Copyright (c) 2016-2026 Baldur Karlsson
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 ******************************************************************************/

#include <d3d11.h>
#include <d3dcompiler.h>
#include <windows.h>
#include <cstdio>
#include "renderdoc/api/app/renderdoc_app.h"
#define CHECK(x)                        \
  if(FAILED(x))                         \
  {                                     \
    printf("failed at %d\n", __LINE__); \
    return 1;                           \
  }
int main(int argc, char **argv)
{
  if(argc != 3)
  {
    printf("Usage: texture_batch_fixture renderdoc.dll capture_prefix\n");
    return 1;
  }
  HMODULE mod = LoadLibraryA(argv[1]);
  if(!mod)
    return 2;
  RENDERDOC_API_1_6_0 *api = nullptr;
  if(!((pRENDERDOC_GetAPI)GetProcAddress(mod, "RENDERDOC_GetAPI"))(eRENDERDOC_API_Version_1_6_0,
                                                                   (void **)&api))
    return 3;
  api->SetCaptureFilePathTemplate(argv[2]);
  ID3D11Device *dev;
  ID3D11DeviceContext *ctx;
  CHECK(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0,
                          D3D11_SDK_VERSION, &dev, nullptr, &ctx));
  const char *vs = "float4 main(float3 pos:POSITION):SV_Position{return float4(pos,1);}";
  const char *ps =
      "Texture2D t:register(t0); Texture2D t2:register(t1); Texture2D bc:register(t2); Texture2D "
      "hdr:register(t3); float4 main():SV_Target{return "
      "t.Load(int3(0,0,0))+t2.Load(int3(0,0,0))+bc.Load(int3(0,0,0))+hdr.Load(int3(0,0,0));}";
  ID3DBlob *v, *p, *err = nullptr;
  CHECK(D3DCompile(vs, strlen(vs), nullptr, nullptr, nullptr, "main", "vs_5_0", 0, 0, &v, &err));
  CHECK(D3DCompile(ps, strlen(ps), nullptr, nullptr, nullptr, "main", "ps_5_0", 0, 0, &p, &err));
  ID3D11VertexShader *vertex;
  ID3D11PixelShader *pixel;
  CHECK(dev->CreateVertexShader(v->GetBufferPointer(), v->GetBufferSize(), nullptr, &vertex));
  CHECK(dev->CreatePixelShader(p->GetBufferPointer(), p->GetBufferSize(), nullptr, &pixel));
  D3D11_INPUT_ELEMENT_DESC input = {
      "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0};
  ID3D11InputLayout *layout;
  CHECK(dev->CreateInputLayout(&input, 1, v->GetBufferPointer(), v->GetBufferSize(), &layout));
  float verts[] = {-0.5f, -0.5f, 0, 0, 0.5f, 0, 0.5f, -0.5f, 0};
  D3D11_BUFFER_DESC bd = {};
  bd.ByteWidth = sizeof(verts);
  bd.Usage = D3D11_USAGE_DEFAULT;
  bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
  D3D11_SUBRESOURCE_DATA init = {verts, 0, 0};
  ID3D11Buffer *vb;
  CHECK(dev->CreateBuffer(&bd, &init, &vb));
  unsigned short indices[] = {0, 1, 2};
  bd.ByteWidth = sizeof(indices);
  bd.BindFlags = D3D11_BIND_INDEX_BUFFER;
  init.pSysMem = indices;
  ID3D11Buffer *ib;
  CHECK(dev->CreateBuffer(&bd, &init, &ib));
  D3D11_TEXTURE2D_DESC td = {};
  td.Width = td.Height = 32;
  td.MipLevels = td.ArraySize = 1;
  td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
  td.SampleDesc.Count = 1;
  td.BindFlags = D3D11_BIND_RENDER_TARGET;
  ID3D11Texture2D *tex;
  CHECK(dev->CreateTexture2D(&td, nullptr, &tex));
  ID3D11RenderTargetView *rt;
  CHECK(dev->CreateRenderTargetView(tex, nullptr, &rt));
  unsigned int pixels[4] = {0xff0000ff, 0xff00ff00, 0xffff0000, 0xffffffff};
  unsigned int mip1 = 0xff123456;
  D3D11_TEXTURE2D_DESC sourceDesc = {};
  sourceDesc.Width = sourceDesc.Height = 2;
  sourceDesc.MipLevels = 2;
  sourceDesc.ArraySize = 1;
  sourceDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
  sourceDesc.SampleDesc.Count = 1;
  sourceDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
  D3D11_SUBRESOURCE_DATA texInit[2] = {{pixels, 8, 16}, {&mip1, 4, 4}};
  ID3D11Texture2D *source;
  CHECK(dev->CreateTexture2D(&sourceDesc, texInit, &source));
  ID3D11ShaderResourceView *srv;
  CHECK(dev->CreateShaderResourceView(source, nullptr, &srv));
  ID3D11ShaderResourceView *views[] = {srv, srv};
  ctx->PSSetShaderResources(0, 2, views);
  unsigned char bcBlock[8] = {0, 248, 0, 0, 0, 0, 0, 0};
  D3D11_TEXTURE2D_DESC bcDesc = sourceDesc;
  bcDesc.Width = bcDesc.Height = 4;
  bcDesc.MipLevels = 1;
  bcDesc.Format = DXGI_FORMAT_BC1_UNORM_SRGB;
  D3D11_SUBRESOURCE_DATA bcInit = {bcBlock, 8, 8};
  ID3D11Texture2D *bcTex;
  CHECK(dev->CreateTexture2D(&bcDesc, &bcInit, &bcTex));
  ID3D11ShaderResourceView *bcView;
  CHECK(dev->CreateShaderResourceView(bcTex, nullptr, &bcView));
  ctx->PSSetShaderResources(2, 1, &bcView);
  float hdrPixels[4] = {2.0f, -0.5f, 0.25f, 1.0f};
  D3D11_TEXTURE2D_DESC hdrDesc = sourceDesc;
  hdrDesc.Width = hdrDesc.Height = 1;
  hdrDesc.MipLevels = 1;
  hdrDesc.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
  D3D11_SUBRESOURCE_DATA hdrInit = {hdrPixels, 16, 16};
  ID3D11Texture2D *hdrTex;
  CHECK(dev->CreateTexture2D(&hdrDesc, &hdrInit, &hdrTex));
  ID3D11ShaderResourceView *hdrView;
  CHECK(dev->CreateShaderResourceView(hdrTex, nullptr, &hdrView));
  ctx->PSSetShaderResources(3, 1, &hdrView);
  D3D11_TEXTURE2D_DESC depthDesc = td;
  depthDesc.Format = DXGI_FORMAT_D32_FLOAT;
  depthDesc.BindFlags = D3D11_BIND_DEPTH_STENCIL;
  ID3D11Texture2D *depthTex;
  CHECK(dev->CreateTexture2D(&depthDesc, nullptr, &depthTex));
  ID3D11DepthStencilView *depthView;
  CHECK(dev->CreateDepthStencilView(depthTex, nullptr, &depthView));
  ctx->ClearDepthStencilView(depthView, D3D11_CLEAR_DEPTH, 0.75f, 0);
  api->StartFrameCapture(dev, nullptr);
  UINT stride = 12, offset = 0;
  ctx->IASetVertexBuffers(0, 1, &vb, &stride, &offset);
  ctx->IASetIndexBuffer(ib, DXGI_FORMAT_R16_UINT, 0);
  ctx->IASetInputLayout(layout);
  ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
  ctx->VSSetShader(vertex, nullptr, 0);
  ctx->PSSetShader(pixel, nullptr, 0);
  ctx->OMSetRenderTargets(1, &rt, depthView);
  D3D11_VIEWPORT vp = {0, 0, 32, 32, 0, 1};
  ctx->RSSetViewports(1, &vp);
  ctx->Draw(3, 0);
  pixels[0] = 0xff00ffff;
  ctx->UpdateSubresource(source, 0, nullptr, pixels, 8, 16);
  verts[0] = -0.25f;
  ctx->UpdateSubresource(vb, 0, nullptr, verts, 0, 0);
  ctx->DrawIndexed(3, 0, 0);
  ctx->DrawInstanced(3, 2, 0, 0);
  if(!api->EndFrameCapture(dev, nullptr))
    return 4;
  char path[2048] = {};
  uint32_t size = sizeof(path);
  api->GetCapture(0, path, &size, nullptr);
  printf("%s\n", path);
  ctx->ClearState();
  ctx->Flush();
  srv->Release();
  source->Release();
  bcView->Release();
  bcTex->Release();
  hdrView->Release();
  hdrTex->Release();
  depthView->Release();
  depthTex->Release();
  rt->Release();
  tex->Release();
  ib->Release();
  vb->Release();
  layout->Release();
  vertex->Release();
  pixel->Release();
  v->Release();
  p->Release();
  ctx->Release();
  dev->Release();
  return 0;
}
