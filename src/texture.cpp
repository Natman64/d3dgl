
#include "texture.hpp"

#include <limits>

#include "d3dgl.hpp"
#include "device.hpp"
#include "adapter.hpp"
#include "trace.hpp"


class D3DGLTextureSurface : public IDirect3DSurface9 {
    std::atomic<ULONG> mRefCount;

    D3DGLTexture *mParent;
    UINT mLevel;

    enum LockType {
        LT_Unlocked,
        LT_ReadOnly,
        LT_Full
    };
    std::atomic<LockType> mLock;
    RECT mLockRegion;

    UINT mDataOffset;
    UINT mDataLength;

    GLubyte *mScratchMem;

public:
    D3DGLTextureSurface(D3DGLTexture *parent, UINT level);
    virtual ~D3DGLTextureSurface();

    void init(UINT offset, UINT length);
    UINT getDataLength() const { return mDataLength; }

    /*** IUnknown methods ***/
    virtual HRESULT WINAPI QueryInterface(REFIID riid, void **obj);
    virtual ULONG WINAPI AddRef();
    virtual ULONG WINAPI Release();
    /*** IDirect3DResource9 methods ***/
    virtual HRESULT WINAPI GetDevice(IDirect3DDevice9 **device);
    virtual HRESULT WINAPI SetPrivateData(REFGUID refguid, const void *data, DWORD size, DWORD flags);
    virtual HRESULT WINAPI GetPrivateData(REFGUID refguid, void *data, DWORD *size);
    virtual HRESULT WINAPI FreePrivateData(REFGUID refguid);
    virtual DWORD WINAPI SetPriority(DWORD priority);
    virtual DWORD WINAPI GetPriority();
    virtual void WINAPI PreLoad();
    virtual D3DRESOURCETYPE WINAPI GetType();
    /*** IDirect3DSurface9 methods ***/
    virtual HRESULT WINAPI GetContainer(REFIID riid, void **container);
    virtual HRESULT WINAPI GetDesc(D3DSURFACE_DESC *desc);
    virtual HRESULT WINAPI LockRect(D3DLOCKED_RECT *lockedRect, const RECT *rect, DWORD flags);
    virtual HRESULT WINAPI UnlockRect();
    virtual HRESULT WINAPI GetDC(HDC *hdc);
    virtual HRESULT WINAPI ReleaseDC(HDC hdc);
};


void D3DGLTexture::initGL()
{
    glActiveTexture(GL_TEXTURE0);
    glGenTextures(1, &mTexId);
    glBindTexture(GL_TEXTURE_2D, mTexId);
    checkGLError();

    glTexImage2D(GL_TEXTURE_2D, 0, mGLFormat->internalformat, mDesc.Width, mDesc.Height, 0,
                 mGLFormat->format, mGLFormat->type, NULL);
    checkGLError();

    if(mDesc.Pool == D3DPOOL_MANAGED)
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_LOD, 0);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, mSurfaces.size()-1);

    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);

    // Force allocation of mipmap levels, if any
    if(mSurfaces.size() > 1)
        glGenerateMipmap(GL_TEXTURE_2D);
    checkGLError();

    UINT total_size = 0;
    GLint w = mDesc.Width;
    GLint h = mDesc.Height;
    for(D3DGLTextureSurface *surface : mSurfaces)
    {
        w = std::max(1, w);
        h = std::max(1, h);

        UINT level_size;
        if(mDesc.Format == D3DFMT_DXT1 || mDesc.Format == D3DFMT_DXT2 ||
           mDesc.Format == D3DFMT_DXT3 || mDesc.Format == D3DFMT_DXT4 ||
           mDesc.Format == D3DFMT_DXT5)
            level_size = ((w+3)/4) * ((h+3)/4) * mGLFormat->bytesperpixel;
        else
            level_size = w*h * mGLFormat->bytesperpixel;

        surface->init(total_size, level_size);
        total_size += level_size;

        w >>= 1;
        h >>= 1;
    }

    /*if((mDesc.Usage&D3DUSAGE_DYNAMIC))
    {
        glGenBuffers(1, &mPBO);
        checkGLError();

        if(mPBO)
        {
            glBindBuffer(GL_PIXEL_PACK_BUFFER, mPBO);
            glBufferData(GL_PIXEL_PACK_BUFFER, total_size, NULL, GL_DYNAMIC_DRAW);
            mUserPtr = glMapBuffer(GL_PIXEL_PACK_BUFFER, GL_READ_WRITE);
            glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
            checkGLError();
        }
    }*/
    if((mDesc.Pool == D3DPOOL_SYSTEMMEM || (mDesc.Usage&D3DUSAGE_DYNAMIC)) && !mPBO)
    {
        mSysMem.resize(total_size);
        mUserPtr = mSysMem.data();
    }

    mUpdateInProgress = 0;
}
class TextureInitCmd : public Command {
    D3DGLTexture *mTarget;

public:
    TextureInitCmd(D3DGLTexture *target) : mTarget(target) { }

    virtual ULONG execute()
    {
        mTarget->initGL();
        return sizeof(*this);
    }
};


void D3DGLTexture::deinitGL()
{
    glDeleteTextures(1, &mTexId);
    glDeleteBuffers(1, &mPBO);
    checkGLError();
}
class TextureDeinitCmd : public Command {
    D3DGLTexture *mTarget;
    HANDLE mFinished;

public:
    TextureDeinitCmd(D3DGLTexture *target, HANDLE finished) : mTarget(target), mFinished(finished) { }

    virtual ULONG execute()
    {
        mTarget->deinitGL();
        SetEvent(mFinished);
        return sizeof(*this);
    }
};


void D3DGLTexture::setLodGL(DWORD lod)
{
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, mTexId);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_LOD, lod);
    checkGLError();
}
class TextureSetLODCmd : public Command {
    D3DGLTexture *mTarget;
    DWORD mLodLevel;

public:
    TextureSetLODCmd(D3DGLTexture *target, DWORD lod)
      : mTarget(target), mLodLevel(lod)
    { }

    virtual ULONG execute()
    {
        mTarget->setLodGL(mLodLevel);
        return sizeof(*this);
    }
};


void D3DGLTexture::genMipmapGL()
{
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, mTexId);
    glGenerateMipmap(GL_TEXTURE_2D);
    checkGLError();
}
class TextureGenMipCmd : public Command {
    D3DGLTexture *mTarget;

public:
    TextureGenMipCmd(D3DGLTexture *target) : mTarget(target) { }

    virtual ULONG execute()
    {
        mTarget->genMipmapGL();
        return sizeof(*this);
    }
};


void D3DGLTexture::loadTexLevelGL(DWORD level, const RECT &rect, GLubyte *dataPtr, bool deletePtr)
{
    UINT w = std::max(1u, mDesc.Width>>level);
    /*UINT h = std::max(1u, mDesc.Height>>Level);*/

    if(mPBO)
    {
        glBindBuffer(GL_PIXEL_UNPACK_BUFFER, mPBO);
        glUnmapBuffer(GL_PIXEL_UNPACK_BUFFER);
        checkGLError();

        dataPtr = (GLubyte*)(dataPtr-mUserPtr);
    }

    D3DGLTextureSurface *surface = mSurfaces[level];

    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, mTexId);
    if(mIsCompressed)
    {
        GLsizei len = -1;
        if(mDesc.Format == D3DFMT_DXT1 || mDesc.Format == D3DFMT_DXT2 ||
           mDesc.Format == D3DFMT_DXT3 || mDesc.Format == D3DFMT_DXT4 ||
           mDesc.Format == D3DFMT_DXT5)
        {
            len  = surface->getDataLength();
            len -= (((rect.top+3)/4)*((w+3)/4) + ((rect.left+3)/4)) *
                   mGLFormat->bytesperpixel;
            dataPtr += ((rect.top/4)*(w/4) + (rect.left/4)) *
                       mGLFormat->bytesperpixel;
            glPixelStorei(GL_UNPACK_ROW_LENGTH, ((w+3)/4)*mGLFormat->bytesperpixel);
        }
        glCompressedTexSubImage2D(GL_TEXTURE_2D, level,
            rect.left, rect.top, rect.right-rect.left, rect.bottom-rect.top,
            mGLFormat->internalformat, len, dataPtr
        );
    }
    else
    {
        dataPtr += (rect.top*w + rect.left) * mGLFormat->bytesperpixel;
        glPixelStorei(GL_UNPACK_ROW_LENGTH, w);
        glTexSubImage2D(GL_TEXTURE_2D, level,
            rect.left, rect.top, rect.right-rect.left, rect.bottom-rect.top,
            mGLFormat->format, mGLFormat->type, dataPtr
        );
    }
    glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);

    if(level == 0 && (mDesc.Usage&D3DUSAGE_AUTOGENMIPMAP) && mSurfaces.size() > 1)
        glGenerateMipmap(GL_TEXTURE_2D);
    checkGLError();

    if(mPBO)
    {
        mUserPtr = (GLubyte*)glMapBuffer(GL_PIXEL_UNPACK_BUFFER, GL_READ_WRITE);
        glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
        checkGLError();
    }

    if(deletePtr)
        delete dataPtr;
    --mUpdateInProgress;
}
class TextureLoadLevelCmd : public Command {
    D3DGLTexture *mTarget;
    DWORD mLevel;
    RECT mRect;
    GLubyte *mDataPtr;
    bool mDeletePtr;

public:
    TextureLoadLevelCmd(D3DGLTexture *target, DWORD level, const RECT &rect, GLubyte *dataPtr, bool deletePtr)
      : mTarget(target), mLevel(level), mRect(rect), mDataPtr(dataPtr), mDeletePtr(deletePtr)
    { }

    virtual ULONG execute()
    {
        mTarget->loadTexLevelGL(mLevel, mRect, mDataPtr, mDeletePtr);
        return sizeof(*this);
    }
};


D3DGLTexture::D3DGLTexture(D3DGLDevice *parent)
  : mRefCount(0)
  , mIfaceCount(0)
  , mParent(parent)
  , mGLFormat(nullptr)
  , mTexId(0)
  , mPBO(0)
  , mUserPtr(nullptr)
  , mDirtyRect({std::numeric_limits<LONG>::max(), std::numeric_limits<LONG>::max(),
                std::numeric_limits<LONG>::min(), std::numeric_limits<LONG>::min()})
  , mUpdateInProgress(1)
  , mLodLevel(0)
{
    mParent->AddRef();
}

D3DGLTexture::~D3DGLTexture()
{
    HANDLE finished = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    mParent->getQueue().send<TextureDeinitCmd>(this, finished);
    WaitForSingleObject(finished, INFINITE);
    CloseHandle(finished);

    for(auto surface : mSurfaces)
        delete surface;
    mSurfaces.clear();

    mParent->Release();
    mParent = nullptr;
}


bool D3DGLTexture::init(const D3DSURFACE_DESC *desc, UINT levels)
{
    mDesc = *desc;

    if(mDesc.Width == 0 || mDesc.Height == 0)
    {
        ERR("Width of height of 0: %ux%u\n", mDesc.Width, mDesc.Height);
        return false;
    }

    auto fmtinfo = gFormatList.find(mDesc.Format);
    if(fmtinfo == gFormatList.end())
    {
        ERR("Failed to find info for format %s\n", d3dfmt_to_str(mDesc.Format));
        return false;
    }
    mGLFormat = &fmtinfo->second;

    if((mDesc.Usage&D3DUSAGE_RENDERTARGET))
    {
        if(mDesc.Pool != D3DPOOL_DEFAULT)
        {
            WARN("RenderTarget not allowed in non-default pool\n");
            return false;
        }
    }
    else if((mDesc.Usage&D3DUSAGE_DEPTHSTENCIL))
    {
        if(mDesc.Pool != D3DPOOL_DEFAULT)
        {
            WARN("DepthStencil target not allowed in non-default pool\n");
            return false;
        }
    }

    if((mDesc.Usage&D3DUSAGE_AUTOGENMIPMAP))
    {
        if(mDesc.Pool == D3DPOOL_SYSTEMMEM)
        {
            WARN("AutoGenMipMap not allowed in systemmem\n");
            return false;
        }
        if(mDesc.Pool == D3DPOOL_MANAGED)
        {
            if(levels > 1)
            {
                WARN("Cannot AutoGenMipMap managed textures\n");
                return false;
            }
            levels = 1;
        }
    }

    UINT maxLevels = 0;
    UINT m = std::max(mDesc.Width, mDesc.Height);
    while(m > 0)
    {
        maxLevels++;
        m >>= 1;
    }
    TRACE("Calculated max mipmap levels: %u\n", maxLevels);

    if(!levels || levels > maxLevels)
        levels = maxLevels;
    for(UINT i = 0;i < levels;++i)
        mSurfaces.push_back(new D3DGLTextureSurface(this, i));

    mIsCompressed = (mDesc.Format == D3DFMT_DXT1 || mDesc.Format == D3DFMT_DXT2 ||
                     mDesc.Format == D3DFMT_DXT3 || mDesc.Format == D3DFMT_DXT4 ||
                     mDesc.Format == D3DFMT_DXT5);
    if(mDesc.Format == D3DFMT_DXT2 || mDesc.Format == D3DFMT_DXT4)
        WARN("Pre-mulitplied alpha textures not supported; loading anyway.");

    mParent->getQueue().send<TextureInitCmd>(this);

    return true;
}

void D3DGLTexture::updateTexture(DWORD level, const RECT &rect, GLubyte *dataPtr, bool deletePtr)
{
    CommandQueue &queue = mParent->getQueue();
    queue.lock();
    ++mUpdateInProgress;
    queue.sendAndUnlock<TextureLoadLevelCmd>(this, level, rect, dataPtr, deletePtr);
}

void D3DGLTexture::addIface()
{
    ++mIfaceCount;
}

void D3DGLTexture::releaseIface()
{
    if(--mIfaceCount == 0)
        delete this;
}


HRESULT D3DGLTexture::QueryInterface(REFIID riid, void **obj)
{
    TRACE("iface %p, riid %s, obj %p\n", this, debugstr_guid(riid), obj);

    *obj = NULL;
#define RETURN_IF_IID_TYPE(obj, riid, TYPE) do { \
    if((riid) == IID_##TYPE)                     \
    {                                            \
        AddRef();                                \
        *(obj) = static_cast<TYPE*>(this);       \
        return D3D_OK;                           \
    }                                            \
} while (0)
    RETURN_IF_IID_TYPE(obj, riid, IDirect3DTexture9);
    RETURN_IF_IID_TYPE(obj, riid, IDirect3DBaseTexture9);
    RETURN_IF_IID_TYPE(obj, riid, IDirect3DResource9);
    RETURN_IF_IID_TYPE(obj, riid, IUnknown);
#undef RETURN_IF_IID_TYPE

    return E_NOINTERFACE;
}

ULONG D3DGLTexture::AddRef()
{
    ULONG ret = ++mRefCount;
    TRACE("%p New refcount: %lu\n", this, ret);
    if(ret == 1) addIface();
    return ret;
}

ULONG D3DGLTexture::Release()
{
    ULONG ret = --mRefCount;
    TRACE("%p New refcount: %lu\n", this, ret);
    if(ret == 0) releaseIface();
    return ret;
}


HRESULT D3DGLTexture::GetDevice(IDirect3DDevice9 **device)
{
    TRACE("iface %p, device %p\n", this, device);
    *device = mParent;
    (*device)->AddRef();
    return D3D_OK;
}

HRESULT D3DGLTexture::SetPrivateData(REFGUID refguid, const void *data, DWORD size, DWORD flags)
{
    FIXME("iface %p, refguid %s, data %p, size %lu, flags 0x%lx : stub!\n", this, debugstr_guid(refguid), data, size, flags);
    return E_NOTIMPL;
}

HRESULT D3DGLTexture::GetPrivateData(REFGUID refguid, void *data, DWORD *size)
{
    FIXME("iface %p, refguid %s, data %p, size %p : stub!\n", this, debugstr_guid(refguid), data, size);
    return E_NOTIMPL;
}

HRESULT D3DGLTexture::FreePrivateData(REFGUID refguid)
{
    FIXME("iface %p, refguid %s : stub!\n", this, debugstr_guid(refguid));
    return E_NOTIMPL;
}

DWORD D3DGLTexture::SetPriority(DWORD priority)
{
    FIXME("iface %p, priority %lu : stub!\n", this, priority);
    return 0;
}

DWORD D3DGLTexture::GetPriority()
{
    FIXME("iface %p : stub!\n", this);
    return 0;
}

void D3DGLTexture::PreLoad()
{
    FIXME("iface %p : stub!\n", this);
}

D3DRESOURCETYPE D3DGLTexture::GetType()
{
    TRACE("iface %p\n", this);
    return D3DRTYPE_TEXTURE;
}


DWORD D3DGLTexture::SetLOD(DWORD lod)
{
    TRACE("iface %p, lod %lu\n", this, lod);

    if(mDesc.Pool != D3DPOOL_MANAGED)
        return 0;

    lod = std::min(lod, (DWORD)mSurfaces.size()-1);

    CommandQueue &queue = mParent->getQueue();
    queue.lock();
    if(mLodLevel.exchange(lod) == lod)
        queue.unlock();
    else
        queue.sendAndUnlock<TextureSetLODCmd>(this, lod);

    return lod;
}

DWORD D3DGLTexture::GetLOD()
{
    TRACE("iface %p\n", this);
    return mLodLevel.load();
}

DWORD D3DGLTexture::GetLevelCount()
{
    TRACE("iface %p\n", this);
    return mSurfaces.size();
}

HRESULT D3DGLTexture::SetAutoGenFilterType(D3DTEXTUREFILTERTYPE type)
{
    FIXME("iface %p, type 0x%x : stub!\n", this, type);
    return D3D_OK;
}

D3DTEXTUREFILTERTYPE D3DGLTexture::GetAutoGenFilterType()
{
    FIXME("iface %p\n", this);
    return D3DTEXF_LINEAR;
}

void D3DGLTexture::GenerateMipSubLevels()
{
    TRACE("iface %p\n", this);
    mParent->getQueue().send<TextureGenMipCmd>(this);
}


HRESULT D3DGLTexture::GetLevelDesc(UINT level, D3DSURFACE_DESC *desc)
{
    TRACE("iface %p, level %u, desc %p\n", this, level, desc);

    if(level >= mSurfaces.size())
    {
        WARN("Level out of range (%u >= %u)\n", level, mSurfaces.size());
        return D3DERR_INVALIDCALL;
    }

    return mSurfaces[level]->GetDesc(desc);
}

HRESULT D3DGLTexture::GetSurfaceLevel(UINT level, IDirect3DSurface9 **surface)
{
    TRACE("iface %p, level %u, surface %p\n", this, level, surface);

    if(level >= mSurfaces.size())
    {
        WARN("Level out of range (%u >= %u)\n", level, mSurfaces.size());
        return D3DERR_INVALIDCALL;
    }

    *surface = mSurfaces[level];
    (*surface)->AddRef();
    return D3D_OK;
}

HRESULT D3DGLTexture::LockRect(UINT level, D3DLOCKED_RECT *lockedRect, const RECT *rect, DWORD flags)
{
    TRACE("iface %p, level %u, lockedRect %p, rect %p, flags 0x%lx\n", this, level, lockedRect, rect, flags);

    if(level >= mSurfaces.size())
    {
        WARN("Level out of range (%u >= %u)\n", level, mSurfaces.size());
        return D3DERR_INVALIDCALL;
    }

    return mSurfaces[level]->LockRect(lockedRect, rect, flags);
}

HRESULT D3DGLTexture::UnlockRect(UINT level)
{
    TRACE("iface %p, level %u\n", this, level);

    if(level >= mSurfaces.size())
    {
        WARN("Level out of range (%u >= %u)\n", level, mSurfaces.size());
        return D3DERR_INVALIDCALL;
    }

    return mSurfaces[level]->UnlockRect();
}

HRESULT D3DGLTexture::AddDirtyRect(const RECT *rect)
{
    TRACE("iface %p, rect %p\n", this, rect);
    mDirtyRect.left = std::min(mDirtyRect.left, rect->left);
    mDirtyRect.top = std::min(mDirtyRect.top, rect->top);
    mDirtyRect.right = std::max(mDirtyRect.right, rect->right);
    mDirtyRect.bottom = std::max(mDirtyRect.bottom, rect->bottom);
    return D3D_OK;
}



D3DGLTextureSurface::D3DGLTextureSurface(D3DGLTexture *parent, UINT level)
  : mRefCount(0)
  , mParent(parent)
  , mLevel(level)
  , mLock(LT_Unlocked)
  , mScratchMem(nullptr)
{
}

D3DGLTextureSurface::~D3DGLTextureSurface()
{
}

void D3DGLTextureSurface::init(UINT offset, UINT length)
{
    mDataOffset = offset;
    mDataLength = length;
}


HRESULT D3DGLTextureSurface::QueryInterface(REFIID riid, void **obj)
{
    TRACE("iface %p, riid %s, obj %p\n", this, debugstr_guid(riid), obj);

    *obj = NULL;
#define RETURN_IF_IID_TYPE(obj, riid, TYPE) do { \
    if((riid) == IID_##TYPE)                     \
    {                                            \
        AddRef();                                \
        *(obj) = static_cast<TYPE*>(this);       \
        return D3D_OK;                           \
    }                                            \
} while (0)
    RETURN_IF_IID_TYPE(obj, riid, IDirect3DSurface9);
    RETURN_IF_IID_TYPE(obj, riid, IDirect3DResource9);
    RETURN_IF_IID_TYPE(obj, riid, IUnknown);
#undef RETURN_IF_IID_TYPE

    return E_NOINTERFACE;
}

ULONG D3DGLTextureSurface::AddRef()
{
    ULONG ret = ++mRefCount;
    TRACE("%p New refcount: %lu\n", this, ret);
    if(ret == 1) mParent->addIface();
    return ret;
}

ULONG D3DGLTextureSurface::Release()
{
    ULONG ret = --mRefCount;
    TRACE("%p New refcount: %lu\n", this, ret);
    if(ret == 0) mParent->releaseIface();
    return ret;
}


HRESULT D3DGLTextureSurface::GetDevice(IDirect3DDevice9 **device)
{
    TRACE("iface %p, device %p\n", this, device);
    return mParent->GetDevice(device);
}

HRESULT D3DGLTextureSurface::SetPrivateData(REFGUID refguid, const void *data, DWORD size, DWORD flags)
{
    FIXME("iface %p, refguid %s, data %p, size %lu, flags 0x%lx : stub!\n", this, debugstr_guid(refguid), data, size, flags);
    return E_NOTIMPL;
}

HRESULT D3DGLTextureSurface::GetPrivateData(REFGUID refguid, void *data, DWORD *size)
{
    FIXME("iface %p, refguid %s, data %p, size %p : stub!\n", this, debugstr_guid(refguid), data, size);
    return E_NOTIMPL;
}

HRESULT D3DGLTextureSurface::FreePrivateData(REFGUID refguid)
{
    FIXME("iface %p, refguid %s : stub!\n", this, debugstr_guid(refguid));
    return E_NOTIMPL;
}

DWORD D3DGLTextureSurface::SetPriority(DWORD priority)
{
    FIXME("iface %p, priority %lu : stub!\n", this, priority);
    return 0;
}

DWORD D3DGLTextureSurface::GetPriority()
{
    FIXME("iface %p : stub!\n", this);
    return 0;
}

void D3DGLTextureSurface::PreLoad()
{
    FIXME("iface %p : stub!\n", this);
}

D3DRESOURCETYPE D3DGLTextureSurface::GetType()
{
    TRACE("iface %p\n", this);
    return D3DRTYPE_SURFACE;
}


HRESULT D3DGLTextureSurface::GetContainer(REFIID riid, void **container)
{
    TRACE("iface %p, riid %s, container %p\n", this, debugstr_guid(riid), container);
    return mParent->QueryInterface(riid, container);
}

HRESULT D3DGLTextureSurface::GetDesc(D3DSURFACE_DESC *desc)
{
    TRACE("iface %p, desc %p\n", this, desc);

    desc->Format = mParent->mDesc.Format;
    desc->Usage = mParent->mDesc.Usage;
    desc->Pool = mParent->mDesc.Pool;
    desc->MultiSampleType = mParent->mDesc.MultiSampleType;
    desc->MultiSampleQuality = mParent->mDesc.MultiSampleQuality;
    desc->Width = std::max(1u, mParent->mDesc.Width>>mLevel);
    desc->Height = std::max(1u, mParent->mDesc.Height>>mLevel);
    return D3D_OK;
}

HRESULT D3DGLTextureSurface::LockRect(D3DLOCKED_RECT *lockedRect, const RECT *rect, DWORD flags)
{
    TRACE("iface %p, lockedRect %p, rect %p, flags 0x%lx\n", this, lockedRect, rect, flags);

    if(mParent->mDesc.Pool == D3DPOOL_DEFAULT && !(mParent->mDesc.Usage&D3DUSAGE_DYNAMIC))
    {
        WARN("Cannot lock non-dynamic textures in default pool\n");
        return D3DERR_INVALIDCALL;
    }

    UINT w = std::max(1u, mParent->mDesc.Width>>mLevel);
    UINT h = std::max(1u, mParent->mDesc.Height>>mLevel);
    RECT full = { 0, 0, (LONG)w, (LONG)h };
    if((flags&D3DLOCK_DISCARD))
    {
        if((flags&D3DLOCK_READONLY))
        {
            WARN("Read-only discard specified\n");
            return D3DERR_INVALIDCALL;
        }
        if(rect)
        {
            WARN("Discardable rect specified\n");
            return D3DERR_INVALIDCALL;
        }
    }
    if(!rect)
        rect = &full;

    {
        LockType lt = ((flags&D3DLOCK_READONLY) ? LT_ReadOnly : LT_Full);
        LockType nolock = LT_Unlocked;
        if(!mLock.compare_exchange_strong(nolock, lt))
        {
            ERR("Texture surface %u already locked!\n", mLevel);
            return D3DERR_INVALIDCALL;
        }
    }

    while(mParent->mUpdateInProgress)
        Sleep(1);

    bool updateMem = false;

    /* NOTE: D3DPOOL_MANAGED resources are lockable, however their main purpose
     * (ensuring resources aren't lost) is already gauranteed by OpenGL. But
     * because they're lockable, we need something the app can write to and
     * read from. Allocating system memory space for this anyway is wasteful
     * (GL already has one), and a pixel buffer object (PBO) would increase
     * VRAM/AGP memory usage just as bad.
     * Instead, temporarilly allocate storage to pass to the app. It will be
     * deallocated after load so as not to hold unnecessary memory. */
    GLubyte *memPtr = mParent->mUserPtr;
    if(memPtr)
        memPtr += mDataOffset;
    else
    {
        if(!mScratchMem)
            mScratchMem = new GLubyte[mDataLength];
        memPtr = mScratchMem;
        updateMem = !(flags&D3DLOCK_DISCARD);
    }

    if(updateMem)
    {
        ERR("Skipping local memory update\n");
    }

    mLockRegion = *rect;
    if(mParent->mIsCompressed)
    {
        memPtr += ((rect->top/4*((w+3)/4)) + (rect->left/4)) * mParent->mGLFormat->bytesperpixel;
        lockedRect->Pitch = (w+3)/4 * mParent->mGLFormat->bytesperpixel;
    }
    else
    {
        memPtr += (rect->top*w + rect->left) * mParent->mGLFormat->bytesperpixel;
        lockedRect->Pitch = w * mParent->mGLFormat->bytesperpixel;
    }
    lockedRect->pBits = memPtr;

    if(!(flags&(D3DLOCK_NO_DIRTY_UPDATE|D3DLOCK_READONLY)))
    {
        RECT dirty = { rect->left<<mLevel, rect->top<<mLevel,
                       rect->right<<mLevel, rect->bottom<<mLevel };
        mParent->AddDirtyRect(&dirty);
    }

    return D3D_OK;
}

HRESULT D3DGLTextureSurface::UnlockRect()
{
    TRACE("iface %p\n", this);

    if(mLock == LT_Unlocked)
    {
        ERR("Attempted to unlock an unlocked surface\n");
        return D3DERR_INVALIDCALL;
    }

    if(mScratchMem)
    {
        mParent->updateTexture(mLevel, mLockRegion, mScratchMem, true);
        mScratchMem = nullptr;
    }
    else
        mParent->updateTexture(mLevel, mLockRegion, mParent->mUserPtr+mDataOffset, false);

    mLock = LT_Unlocked;
    return D3D_OK;
}

HRESULT D3DGLTextureSurface::GetDC(HDC *hdc)
{
    FIXME("iface %p, hdc %p : stub!\n", this, hdc);
    return E_NOTIMPL;
}

HRESULT D3DGLTextureSurface::ReleaseDC(HDC hdc)
{
    FIXME("iface %p, hdc %p : stub!\n", this, hdc);
    return E_NOTIMPL;
}