#include <donut/render/DeferredLightingPass.h>
#include <donut/render/DrawStrategy.h>
#include <donut/render/GBuffer.h>
#include <donut/render/GBufferFillPass.h>

#include <donut/app/ApplicationBase.h>
#include <donut/app/Camera.h>
#include <donut/app/DeviceManager.h>

#include <donut/engine/BindingCache.h>
#include <donut/engine/CommonRenderPasses.h>
#include <donut/engine/Scene.h>
#include <donut/engine/ShaderFactory.h>
#include <donut/engine/TextureCache.h>
#include <donut/engine/View.h>

#include <donut/core/math/vector.h>

using namespace donut::render;
using namespace donut::math;
using namespace donut;

#define _STRINGIFY(s) #s
#define STRINGIFY(s)  _STRINGIFY(s)

class RenderTargets : public GBufferRenderTargets {
public:
    nvrhi::TextureHandle shadedColor;

    void Init(nvrhi::IDevice* device, dm::uint2 size, dm::uint sampleCount, bool enableMotionVectors, bool useReverseProjection) override {
        GBufferRenderTargets::Init(device, size, sampleCount, enableMotionVectors, useReverseProjection);

        nvrhi::TextureDesc textureDesc;
        textureDesc.dimension = nvrhi::TextureDimension::Texture2D;
        textureDesc.initialState = nvrhi::ResourceStates::UnorderedAccess;
        textureDesc.keepInitialState = true;
        textureDesc.debugName = "ShadedColor";
        textureDesc.isUAV = true;
        textureDesc.format = nvrhi::Format::RGBA16_FLOAT;
        textureDesc.width = size.x;
        textureDesc.height = size.y;
        textureDesc.sampleCount = sampleCount;
        shadedColor = device->createTexture(textureDesc);
    }
};

class DeferredRendering : public app::ApplicationBase {
private:
    std::shared_ptr<vfs::RootFileSystem> m_RootFS;
    nvrhi::CommandListHandle m_CommandList;

    std::unique_ptr<engine::Scene> m_Scene;
    std::shared_ptr<engine::ShaderFactory> m_ShaderFactory;
    std::unique_ptr<engine::BindingCache> m_BindingCache;

    std::shared_ptr<RenderTargets> m_RenderTargets;
    std::unique_ptr<GBufferFillPass> m_GBufferFillPass;
    std::unique_ptr<DeferredLightingPass> m_DeferredLightingPass;

    std::shared_ptr<InstancedOpaqueDrawStrategy> m_OpaqueDrawStrategy;

    app::FirstPersonCamera m_Camera;
    engine::PlanarView m_View;

public:
    using ApplicationBase::ApplicationBase;

    bool Init() {
        std::filesystem::path sceneFileName
            = app::GetDirectoryWithExecutable().parent_path() / "media/glTF-Sample-Assets/Models/Sponza/glTF/Sponza.gltf";
        std::filesystem::path frameworkShaderPath
            = app::GetDirectoryWithExecutable() / "shaders/framework" / app::GetShaderTypeName(GetDevice()->getGraphicsAPI());

        m_RootFS = std::make_shared<vfs::RootFileSystem>();
        m_RootFS->mount("/shaders/donut", frameworkShaderPath);

        m_ShaderFactory = std::make_shared<engine::ShaderFactory>(GetDevice(), m_RootFS, "/shaders");
        m_CommonPasses = std::make_shared<engine::CommonRenderPasses>(GetDevice(), m_ShaderFactory);
        m_BindingCache = std::make_unique<engine::BindingCache>(GetDevice());

        auto nativeFS = std::make_shared<vfs::NativeFileSystem>();
        m_TextureCache = std::make_shared<engine::TextureCache>(GetDevice(), nativeFS, nullptr);

        SetAsynchronousLoadingEnabled(false);
        BeginLoadingScene(nativeFS, sceneFileName);

        m_Scene->FinishedLoading(GetFrameIndex());

        m_Camera.LookAt(dm::float3(0.f, 1.8f, 0.f), dm::float3(1.f, 1.8f, 0.f));
        m_Camera.SetMoveSpeed(3.f);

        m_CommandList = GetDevice()->createCommandList();

        m_DeferredLightingPass = std::make_unique<DeferredLightingPass>(GetDevice(), m_CommonPasses);
        m_DeferredLightingPass->Init(m_ShaderFactory);

        GBufferFillPass::CreateParameters GBufferParams;
        m_GBufferFillPass = std::make_unique<GBufferFillPass>(GetDevice(), m_CommonPasses);
        m_GBufferFillPass->Init(*m_ShaderFactory, GBufferParams);

        m_OpaqueDrawStrategy = std::make_shared<InstancedOpaqueDrawStrategy>();

        CreateRenderTargets();

        return true;
    }

    void CreateRenderTargets() {
        m_RenderTargets = std::make_shared<RenderTargets>();
        int w, h;
        GetDeviceManager()->GetWindowDimensions(w, h);
        m_RenderTargets->Init(GetDevice(), {(uint)w, (uint)h}, 1, false, true);
    }

    bool LoadScene(std::shared_ptr<vfs::IFileSystem> fs, const std::filesystem::path& sceneFileName) override {
        std::unique_ptr<engine::Scene> scene = std::make_unique<engine::Scene>(GetDevice(), *m_ShaderFactory, fs, m_TextureCache, nullptr, nullptr);

        if (scene->Load(sceneFileName)) {
            m_Scene = std::move(scene);
            return true;
        }
        return false;
    }

    bool KeyboardUpdate(int key, int scancode, int action, int mods) override {
        m_Camera.KeyboardUpdate(key, scancode, action, mods);

        return true;
    }

    bool MouseScrollUpdate(double xoffset, double yoffset) override {
        return false;
    }

    bool MousePosUpdate(double xpos, double ypos) override {
        m_Camera.MousePosUpdate(xpos, ypos);
        return true;
    }

    bool MouseButtonUpdate(int button, int action, int mods) override {
        m_Camera.MouseButtonUpdate(button, action, mods);
        return true;
    }

    void Animate(float fElapsedTimeSeconds) override {
        m_Camera.Animate(fElapsedTimeSeconds);
    }

    void BackBufferResizing() override {
    }

    void Render(nvrhi::IFramebuffer* framebuffer) override {
        const nvrhi::FramebufferInfoEx& fbinfo = framebuffer->getFramebufferInfo();

        uint2 size = uint2(fbinfo.width, fbinfo.height);

        if (!m_RenderTargets || any(m_RenderTargets->GetSize() != size)) {
            m_BindingCache->Clear();
            m_DeferredLightingPass->ResetBindingCache();

            m_GBufferFillPass->ResetBindingCache();

            CreateRenderTargets();
        }

        nvrhi::Viewport windowViewport(float(fbinfo.width), float(fbinfo.height));
        m_View.SetViewport(windowViewport);
        m_View.SetMatrices(
            m_Camera.GetWorldToViewMatrix(), perspProjD3DStyleReverse(dm::PI_f * 0.25f, windowViewport.width() / windowViewport.height(), 0.1f));
        m_View.UpdateCache();

        m_CommandList->open();

        m_RenderTargets->Clear(m_CommandList);

        GBufferFillPass::Context context;
        RenderCompositeView(m_CommandList, &m_View, &m_View, *m_RenderTargets->GBufferFramebuffer, m_Scene->GetSceneGraph()->GetRootNode(),
            *(m_OpaqueDrawStrategy.get()), *m_GBufferFillPass, context, "GBufferPass", false);

        DeferredLightingPass::Inputs deferredInputs;
        deferredInputs.SetGBuffer(*m_RenderTargets);
        deferredInputs.lights = &m_Scene->GetSceneGraph()->GetLights();
        deferredInputs.ambientColorTop = 1.0f;
        deferredInputs.ambientColorBottom = deferredInputs.ambientColorTop * float3(0.3f, 0.4f, 0.3f);
        deferredInputs.output = m_RenderTargets->shadedColor;

        m_DeferredLightingPass->Render(m_CommandList, m_View, deferredInputs);

        m_CommonPasses->BlitTexture(m_CommandList, framebuffer, m_RenderTargets->shadedColor, m_BindingCache.get());
        m_CommandList->close();

        GetDevice()->executeCommandList(m_CommandList);
    }
};

#ifdef WIN32
int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nCmdShow)
#else
int main(int __argc, const char** __argv)
#endif
{
    nvrhi::GraphicsAPI api = app::GetGraphicsAPIFromCommandLine(__argc, __argv);
    if (api == nvrhi::GraphicsAPI::D3D11) {
        log::error("The Threaded Rendering example does not support D3D11.");
        return 1;
    }

    app::DeviceManager* deviceManager = app::DeviceManager::Create(api);

    app::DeviceCreationParameters deviceParams;
    deviceParams.backBufferWidth = 1024;
    deviceParams.backBufferHeight = 1024;
#ifdef _DEBUG
    deviceParams.enableDebugRuntime = true;
    deviceParams.enableNvrhiValidationLayer = true;
#endif

    std::string windowTitle = STRINGIFY(PROJECT_NAME);

    if (!deviceManager->CreateWindowDeviceAndSwapChain(deviceParams, windowTitle.c_str())) {
        log::fatal("Cannot initialize a graphics device with the requested parameters");
        return 1;
    }

    {
        DeferredRendering example(deviceManager);
        if (example.Init()) {
            deviceManager->AddRenderPassToBack(&example);
            deviceManager->RunMessageLoop();
            deviceManager->RemoveRenderPass(&example);
        }
    }

    deviceManager->Shutdown();

    delete deviceManager;

    return 0;
}