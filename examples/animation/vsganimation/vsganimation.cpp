#include <vsg/all.h>

#ifdef vsgXchange_FOUND
#    include <vsgXchange/all.h>
#endif

#ifdef Tracy_FOUND
#    include <vsg/utils/TracyInstrumentation.h>
#endif

#include <iostream>


class AnimationControl : public vsg::Inherit<vsg::Visitor, AnimationControl>
{
public:

    AnimationControl(vsg::ref_ptr<vsg::AnimationManager> am, const vsg::Animations& in_animations) : animationManager(am), animations(in_animations)
    {
        itr = animations.end();
    }

    vsg::ref_ptr<vsg::AnimationManager> animationManager;
    vsg::Animations animations;
    vsg::Animations::iterator itr;

    void apply(vsg::KeyPressEvent& keyPress) override
    {
        if (animations.empty()) return;

        if (keyPress.keyModified == 'p')
        {
            keyPress.handled = true;

            // if the iterator hasn't been set yet, set it to the next animation after the last one played,
            // or the first entry in the list of animations
            if (itr == animations.end())
            {
                if (!(animationManager->animations.empty()))
                {
                    itr = std::find(animations.begin(), animations.end(), animationManager->animations.back());
                    if (itr != animations.end()) ++itr;
                }

                if (itr == animations.end()) itr = animations.begin();
            }

            // stop all running animations
            animationManager->stop();

            // play the animation
            if (itr != animations.end())
            {
                if (animationManager->play(*itr))
                {
                    std::cout<<"Playing "<<(*itr)->name<<std::endl;

                    ++itr;
                    if (itr == animations.end()) itr = animations.begin();
                }
            }
        }
        else if (keyPress.keyModified == 's')
        {
            keyPress.handled = true;

            // stop all running animations
            animationManager->stop();
        }
    }
};

int main(int argc, char** argv)
{
    // set up defaults and read command line arguments to override them
    vsg::CommandLine arguments(&argc, argv);

    // set up defaults and read command line arguments to override them
    auto options = vsg::Options::create();
    options->sharedObjects = vsg::SharedObjects::create();
    options->fileCache = vsg::getEnv("VSG_FILE_CACHE");
    options->paths = vsg::getEnvPaths("VSG_FILE_PATH");

#ifdef vsgXchange_all
    // add vsgXchange's support for reading and writing 3rd party file formats
    options->add(vsgXchange::all::create());
#endif

    arguments.read(options);

    auto windowTraits = vsg::WindowTraits::create();
    windowTraits->windowTitle = "vsganimation";

    windowTraits->debugLayer = arguments.read({"--debug", "-d"});
    windowTraits->apiDumpLayer = arguments.read({"--api", "-a"});

    arguments.read("--screen", windowTraits->screenNum);
    arguments.read("--display", windowTraits->display);
    auto numFrames = arguments.value(-1, "-f");
    if (arguments.read({"--fullscreen", "--fs"})) windowTraits->fullscreen = true;
    if (arguments.read({"--window", "-w"}, windowTraits->width, windowTraits->height)) { windowTraits->fullscreen = false; }
    if (arguments.read("--IMMEDIATE")) { windowTraits->swapchainPreferences.presentMode = VK_PRESENT_MODE_IMMEDIATE_KHR; }
    if (arguments.read({"-t", "--test"}))
    {
        windowTraits->swapchainPreferences.presentMode = VK_PRESENT_MODE_IMMEDIATE_KHR;
        windowTraits->fullscreen = true;
    }
    if (arguments.read("--st"))
    {
        windowTraits->swapchainPreferences.presentMode = VK_PRESENT_MODE_IMMEDIATE_KHR;
        windowTraits->width = 192, windowTraits->height = 108;
        windowTraits->decoration = false;
    }

    vsg::ref_ptr<vsg::Instrumentation> instrumentation;
    if (arguments.read({"--gpu-annotation", "--ga"}) && vsg::isExtensionSupported(VK_EXT_DEBUG_UTILS_EXTENSION_NAME))
    {
        windowTraits->debugUtils = true;

        auto gpu_instrumentation = vsg::GpuAnnotation::create();
        if (arguments.read("--func")) gpu_instrumentation->labelType = vsg::GpuAnnotation::SourceLocation_function;

        instrumentation = gpu_instrumentation;
    }
#ifdef Tracy_FOUND
    else if (arguments.read("--tracy"))
    {
        windowTraits->deviceExtensionNames.push_back(VK_EXT_CALIBRATED_TIMESTAMPS_EXTENSION_NAME);

        auto tracy_instrumentation = vsg::TracyInstrumentation::create();
        arguments.read("--cpu", tracy_instrumentation->settings->cpu_instumentation_level);
        arguments.read("--gpu", tracy_instrumentation->settings->gpu_instumentation_level);
        instrumentation = tracy_instrumentation;
    }
#endif

    // when using shadows set the depthClampEnable to true;
    bool depthClamp = true;
    if (depthClamp)
    {
        auto rasterizationState = vsg::RasterizationState::create();
        rasterizationState->depthClampEnable = VK_TRUE;

        auto pbr = options->shaderSets["pbr"] = vsg::createPhysicsBasedRenderingShaderSet(options);
        pbr->defaultGraphicsPipelineStates.push_back(rasterizationState);

        auto phong = options->shaderSets["phong"] = vsg::createPhysicsBasedRenderingShaderSet(options);
        phong->defaultGraphicsPipelineStates.push_back(rasterizationState);

        auto flat = options->shaderSets["flat"] = vsg::createPhysicsBasedRenderingShaderSet(options);
        flat->defaultGraphicsPipelineStates.push_back(rasterizationState);
    }

    auto autoPlay = !arguments.read({"--no-auto-play", "-n"});

    auto outputFilename = arguments.value<vsg::Path>("", "-o");

    if (argc<=1)
    {
        std::cout<<"Please specify model to load on command line."<<std::endl;
        return 0;
    }

    vsg::Path filename = argv[1];
    auto model = vsg::read_cast<vsg::Node>(filename, options);
    if (!model)
    {
        std::cout<<"Failed to load "<<filename<<std::endl;
        return 1;
    }

    auto animations = vsg::visit<vsg::FindAnimations>(model).animations;

    std::cout<<"Model contains "<<animations.size()<<" animations."<<std::endl;
    for(auto animation : animations)
    {
        std::cout<<"    "<<animation->name<<std::endl;
    }

    // compute the bounds of the scene graph to help position camera
    auto bounds = vsg::visit<vsg::ComputeBounds>(model).bounds;
    vsg::dvec3 centre = (bounds.min + bounds.max) * 0.5;
    double radius = vsg::length(bounds.max - bounds.min) * 0.6;

    auto scene = vsg::Group::create();
    scene->addChild(model);

    bool insertBaseGeometry = true;
    if (insertBaseGeometry)
    {
        auto builder = vsg::Builder::create();
        builder->options = options;

        vsg::GeometryInfo geomInfo;
        vsg::StateInfo stateInfo;

        double diameter = vsg::length(bounds.max - bounds.min);
        geomInfo.position.set((bounds.min.x + bounds.max.x)*0.5, (bounds.min.y + bounds.max.y)*0.5, bounds.min.z);
        geomInfo.dx.set(1.5*diameter, 0.0, 0.0);
        geomInfo.dy.set(0.0, 1.5*diameter, 0.0);
        geomInfo.color.set(1.0f, 1.0f, 1.0f, 1.0f);

        stateInfo.two_sided = true;

        scene->addChild(builder->createQuad(geomInfo, stateInfo));
    }

    unsigned int numLights = 2;
    auto direction = arguments.value(vsg::dvec3(-1.0, 1.0, -1.0), "--direction");
    auto numShadowMapsPerLight = arguments.value<uint32_t>(3, "--sm");
    double maxShadowDistance = 1e8;
    if (numLights >= 1)
    {
        auto directionalLight = vsg::DirectionalLight::create();
        directionalLight->name = "directional";
        directionalLight->color.set(1.0, 1.0, 1.0);
        directionalLight->intensity = 0.9;
        directionalLight->direction = direction;
        directionalLight->shadowMaps = numShadowMapsPerLight;
        scene->addChild(directionalLight);

        auto ambientLight = vsg::AmbientLight::create();
        ambientLight->name = "ambient";
        ambientLight->color.set(1.0, 1.0, 1.0);
        ambientLight->intensity = 0.1;
        scene->addChild(ambientLight);
    }

    // write out scene if required
    if (outputFilename)
    {
        vsg::write(scene, outputFilename, options);
        return 0;
    }


    // create the viewer and assign window(s) to it
    auto viewer = vsg::Viewer::create();

    auto window = vsg::Window::create(windowTraits);
    if (!window)
    {
        std::cout << "Could not create window." << std::endl;
        return 1;
    }

    viewer->addWindow(window);

    vsg::ref_ptr<vsg::LookAt> lookAt;

    // update bounds to new scene extent
    bounds = vsg::visit<vsg::ComputeBounds>(scene).bounds;
    centre = (bounds.min + bounds.max) * 0.5;
    radius = vsg::length(bounds.max - bounds.min) * 0.6;
    maxShadowDistance = radius * 4.0;

    // set up the camera
    lookAt = vsg::LookAt::create(centre + vsg::dvec3(0.0, -radius * 3.5, 0.0), centre, vsg::dvec3(0.0, 0.0, 1.0));

    double nearFarRatio = 0.001;
    auto perspective = vsg::Perspective::create(30.0, static_cast<double>(window->extent2D().width) / static_cast<double>(window->extent2D().height), nearFarRatio * radius, radius * 10.0);

    auto camera = vsg::Camera::create(perspective, lookAt, vsg::ViewportState::create(window->extent2D()));

    // add the camera and scene graph to View
    auto view = vsg::View::create();
    view->viewDependentState->maxShadowDistance = maxShadowDistance;
    view->camera = camera;
    view->addChild(scene);

    // add close handler to respond to the close window button and pressing escape
    viewer->addEventHandler(vsg::CloseHandler::create(viewer));
    viewer->addEventHandler(vsg::Trackball::create(camera));
    viewer->addEventHandler(AnimationControl::create(viewer->animationManager, animations));

    auto renderGraph = vsg::RenderGraph::create(window, view);
    auto commandGraph = vsg::CommandGraph::create(window, renderGraph);
    viewer->assignRecordAndSubmitTaskAndPresentation({commandGraph});

    if (instrumentation) viewer->assignInstrumentation(instrumentation);

    viewer->compile();

    // start first animation if available
    if (autoPlay && !animations.empty()) viewer->animationManager->play(animations.front());


    auto startTime = vsg::clock::now();
    double numFramesCompleted = 0.0;

    // rendering main loop
    while (viewer->advanceToNextFrame() && (numFrames < 0 || (numFrames--) > 0))
    {
        // pass any events into EventHandlers assigned to the Viewer
        viewer->handleEvents();
        viewer->update();
        viewer->recordAndSubmit();
        viewer->present();

        numFramesCompleted += 1.0;
    }

    auto duration = std::chrono::duration<double, std::chrono::seconds::period>(vsg::clock::now() - startTime).count();
    if (numFramesCompleted > 0.0)
    {
        std::cout << "Average frame rate = " << (numFramesCompleted / duration) << std::endl;
    }

    return 0;
}
