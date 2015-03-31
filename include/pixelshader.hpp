#ifndef PIXELSHADER_HPP
#define PIXELSHADER_HPP

#include <atomic>
#include <vector>
#include <d3d9.h>


class D3DGLDevice;

class D3DGLPixelShader : public IDirect3DPixelShader9 {
    std::atomic<ULONG> mRefCount;

    D3DGLDevice *mParent;

    std::vector<BYTE> mCode;

public:
    D3DGLPixelShader(D3DGLDevice *parent);
    virtual ~D3DGLPixelShader();

    bool init(const DWORD *data);

    /*** IUnknown methods ***/
    virtual HRESULT WINAPI QueryInterface(REFIID riid, void **obj);
    virtual ULONG WINAPI AddRef();
    virtual ULONG WINAPI Release();
    /*** IDirect3DPixelShader9 methods ***/
    virtual HRESULT WINAPI GetDevice(IDirect3DDevice9 **device);
    virtual HRESULT WINAPI GetFunction(void *data, UINT *size);
};

#endif /* PIXELSHADER_HPP */
