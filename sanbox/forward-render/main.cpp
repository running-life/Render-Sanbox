#include <donut/app/ApplicationBase.h>
#include <donut/app/Camera.h>
#include <donut/app/DeviceManager.h>
#include <donut/core/log.h>
#include <donut/core/math/math.h>
#include <donut/core/vfs/VFS.h>
#include <donut/engine/BindingCache.h>
#include <donut/engine/CommonRenderPasses.h>
#include <donut/engine/FramebufferFactory.h>
#include <donut/engine/Scene.h>
#include <donut/engine/ShaderFactory.h>
#include <donut/engine/TextureCache.h>
#include <donut/render/DrawStrategy.h>
#include <donut/render/ForwardShadingPass.h>

using namespace donut;
using namespace donut::math;

#define _STRINGIFY(s) #s
#define STRINGIFY(s)  _STRINGIFY(s)

class ForwardRendering : public app::ApplicationBase {
public:
    std::shared_ptr<vfs::RootFileSystem> m_RootFS;
    nvrhi::CommandListHandle m_CommandList;

    nvrhi::TextureHandle m_DepthBuffer;
    nvrhi::TextureHandle m_ColorBuffer;
    std::unique_ptr<engine::FramebufferFactory> m_Framebuffer;

    std::unique_ptr<render::ForwardShadingPass> m_ForwardShadingPass;
    std::shared_ptr<engine::ShaderFactory> m_ShaderFactory;
    std::unique_ptr<engine::Scene> m_Scene;
    std::unique_ptr<engine::BindingCache> m_BindingCache;

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

        m_ForwardShadingPass = std::make_unique<render::ForwardShadingPass>(GetDevice(), m_CommonPasses);
        render::ForwardShadingPass::CreateParameters forwardParams;
        forwardParams.numConstantBufferVersions = 128;
        m_ForwardShadingPass->Init(*m_ShaderFactory, forwardParams);

        CreateRenderTargets();

        return true;
    }

    void CreateRenderTargets() {
        int w, h;
        GetDeviceManager()->GetWindowDimensions(w, h);

        auto textureDesc = nvrhi::TextureDesc()
                               .setDimension(nvrhi::TextureDimension::Texture2D)
                               .setArraySize(1)
                               .setWidth(uint32_t(w))
                               .setHeight(uint32_t(h))
                               .setClearValue(nvrhi::Color(0.f))
                               .setIsRenderTarget(true)
                               .setKeepInitialState(true);

        m_ColorBuffer = GetDevice()->createTexture(
            textureDesc.setDebugName("ColorBuffer").setFormat(nvrhi::Format::SRGBA8_UNORM).setInitialState(nvrhi::ResourceStates::RenderTarget));

        m_DepthBuffer = GetDevice()->createTexture(
            textureDesc.setDebugName("DepthBuffer").setFormat(nvrhi::Format::D32).setInitialState(nvrhi::ResourceStates::DepthWrite));

        m_Framebuffer = std::make_unique<engine::FramebufferFactory>(GetDevice());
        m_Framebuffer->RenderTargets.push_back(m_ColorBuffer);
        m_Framebuffer->DepthTarget = m_DepthBuffer;
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
        if (!m_Scene) {
            return;
        }

        const auto& fbinfo = framebuffer->getFramebufferInfo();
        {
            uint2 size = uint2(fbinfo.width, fbinfo.height);
            uint2 size2 = uint2(m_ColorBuffer->getDesc().width, m_ColorBuffer->getDesc().height);

            if (!m_ColorBuffer || any(size2 != size)) {
                m_BindingCache->Clear();
                m_ForwardShadingPass->ResetBindingCache();
                CreateRenderTargets();
            }
        }

        nvrhi::Viewport windowViewport(float(fbinfo.width), float(fbinfo.height));
        m_View.SetViewport(windowViewport);
        m_View.SetMatrices(
            m_Camera.GetWorldToViewMatrix(), perspProjD3DStyleReverse(dm::PI_f * 0.25f, windowViewport.width() / windowViewport.height(), 0.1f));
        m_View.UpdateCache();

        m_CommandList->open();

        m_CommandList->clearTextureFloat(m_ColorBuffer, nvrhi::AllSubresources, nvrhi::Color(0.0f));
        m_CommandList->clearDepthStencilTexture(m_DepthBuffer, nvrhi::AllSubresources, true, 0.f, false, 0);

        render::ForwardShadingPass::Context context;
        m_ForwardShadingPass->PrepareLights(context, m_CommandList, {}, 1.0f, 0.3f, {});

        m_CommandList->setEnableAutomaticBarriers(false);
        m_CommandList->setResourceStatesForFramebuffer(m_Framebuffer->GetFramebuffer(m_View));
        m_CommandList->commitBarriers();

        render::InstancedOpaqueDrawStrategy strategy;
        render::RenderCompositeView(
            m_CommandList, &m_View, &m_View, *m_Framebuffer, m_Scene->GetSceneGraph()->GetRootNode(), strategy, *m_ForwardShadingPass, context);

        m_CommandList->setEnableAutomaticBarriers(true);

        {
            engine::BlitParameters bp;
            bp.targetFramebuffer = framebuffer;
            bp.targetViewport = windowViewport;
            bp.sourceTexture = m_ColorBuffer;
            bp.sourceMip = 0;
            m_CommonPasses->BlitTexture(m_CommandList, bp, m_BindingCache.get());
        }

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
        ForwardRendering example(deviceManager);
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