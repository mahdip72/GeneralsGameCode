set(WW3D2_SRC
    aabtree.cpp
    aabtree.h
    #aabtreebuilder.cpp
    #aabtreebuilder.h
    agg_def.cpp
    agg_def.h
    animatedsoundmgr.cpp
    animatedsoundmgr.h
    #animobj.cpp
    #animobj.h
    #assetmgr.cpp
    #assetmgr.h
    assetstatus.cpp
    assetstatus.h
    bitmaphandler.cpp
    bitmaphandler.h
    bmp2d.cpp
    bmp2d.h
    #boxrobj.cpp
    #boxrobj.h
    bwrender.cpp
    bwrender.h
    #camera.cpp
    #camera.h
    #classid.h # unused
    collect.cpp
    collect.h
    colorspace.h
    coltest.cpp
    coltest.h
    coltype.h
    composite.cpp
    composite.h
    #dazzle.cpp
    #dazzle.h
    #ddsfile.cpp
    #ddsfile.h
    #decalmsh.cpp
    #decalmsh.h
    decalsys.cpp
    decalsys.h
    distlod.cpp
    distlod.h
    dllist.h
    dx8fvf.h
    dx8indexbuffer.h
    dx8list.h
    dx8vertexbuffer.h
    dynamesh.cpp
    dynamesh.h
    font3d.cpp
    font3d.h
    formconv.h
    FramGrab.cpp
    framgrab.h
    hanim.cpp
    hanim.h
    #hanimmgr.cpp
    #hanimmgr.h
    hcanim.cpp
    hcanim.h
    #hlod.cpp
    #hlod.h
    hmdldef.cpp
    hmdldef.h
    #hmorphanim.cpp
    #hmorphanim.h
    #hrawanim.cpp
    #hrawanim.h
    htree.cpp
    htree.h
    #htreemgr.cpp
    #htreemgr.h
    intersec.cpp
    intersec.h
    intersec.inl
    inttest.h
    layer.cpp
    layer.h
    #light.cpp
    #light.h
    #lightenvironment.cpp
    #lightenvironment.h
    line3d.h
    #linegrp.cpp
    #linegrp.h
    #mapper.cpp
    #mapper.h
    matinfo.cpp
    matinfo.h
    matpass.cpp
    matpass.h
    #matrixmapper.cpp
    #matrixmapper.h
    #mesh.cpp
    #mesh.h
    #meshbuild.cpp
    #meshbuild.h
    meshdam.cpp
    meshdam.h
    #meshgeometry.cpp
    #meshgeometry.h
    #meshmatdesc.cpp
    #meshmatdesc.h
    #meshmdl.cpp
    #meshmdl.h
    #meshmdlio.cpp
    metalmap.cpp
    metalmap.h
    missingtexture.h
    #motchan.cpp
    #motchan.h
    #nullrobj.cpp
    #nullrobj.h
    #part_buf.cpp
    #part_buf.h
    #part_emt.cpp
    #part_emt.h
    #part_ldr.cpp
    #part_ldr.h
    pivot.cpp
    pivot.h
    pointgr.cpp
    pointgr.h
    polyinfo.cpp
    polyinfo.h
    predlod.cpp
    predlod.h
    prim_anim.cpp
    prim_anim.h
    projector.cpp
    projector.h
    proto.cpp
    proto.h
    proxy.h
    rddesc.h
    #render2d.cpp
    #render2d.h
    render2dsentence.cpp
    render2dsentence.h
    renderobjectrecycler.cpp
    renderobjectrecycler.h
    rendobj.cpp
    rendobj.h
    #rinfo.cpp
    #rinfo.h
    ringobj.cpp
    ringobj.h
    robjlist.h
    #scene.cpp
    #scene.h
    segline.cpp
    segline.h
    seglinerenderer.cpp
    seglinerenderer.h
    #shader.cpp
    #shader.h
    shattersystem.cpp
    shattersystem.h
    shdlib.h
    snappts.cpp
    snapPts.h
    sortingrenderer.h
    soundlibrarybridge.h
    # The legacy sound render object owns WWAudio/Miles objects and is used by
    # authoring tools, not by the game runtime. It is appended for x86 below.
    sphereobj.cpp
    sphereobj.h
    static_sort_list.cpp
    static_sort_list.h
    statistics.cpp
    statistics.h
    streak.cpp
    streak.h
    streakRender.cpp
    streakRender.h
    stripoptimizer.cpp
    stripoptimizer.h
    surfaceclass.h
    surfaceblit.h
    texproject.cpp
    texproject.h
    #textdraw.cpp # unused
    textdraw.h
    texture.h
    texturefilter.cpp
    texturefilter.h
    textureloader.h
    texturemipbuffer.cpp
    texturemipbuffer.h
    texturemipgenerator.h
    texturethumbnail.cpp
    texturethumbnail.h
    #vertmaterial.cpp
    #vertmaterial.h
    visrasterizer.cpp
    visrasterizer.h
    w3d_dep.cpp
    w3d_dep.h
    #w3d_file.h
    w3d_obsolete.h
    w3d_util.cpp
    w3d_util.h
    w3derr.h
    w3dexclusionlist.cpp
    w3dexclusionlist.h
    ww3d.cpp
    ww3d.h
    ww3dformat.h
    ww3dids.h
    ww3dtrig.h
)

if(CMAKE_SIZEOF_VOID_P EQUAL 8)
    # These implementations are the device-neutral product path. The x86
    # compatibility lane gets the original implementations from the external
    # legacy-renderer targets wired by the top-level build graph.
    list(APPEND WW3D2_SRC
        formconv.cpp
        missingtexture.cpp
        sortingrenderer.cpp
        surfaceclass.cpp
        surfaceblit.cpp
        texture.cpp
        textureloader.cpp
        texturemipgenerator.cpp
        ww3dformat.cpp
        gamewebbrowser.cpp
    )
elseif(CMAKE_SIZEOF_VOID_P EQUAL 4)
    list(APPEND WW3D2_SRC
        soundrobj.cpp
        soundrobj.h
    )
endif()

add_library(corei_ww3d2 INTERFACE)

target_sources(corei_ww3d2 INTERFACE ${WW3D2_SRC})

target_include_directories(corei_ww3d2 INTERFACE
    "${CMAKE_CURRENT_SOURCE_DIR}"
)

if(CMAKE_SIZEOF_VOID_P EQUAL 4)
    target_compile_definitions(corei_ww3d2 INTERFACE
        RTS_ENABLE_LEGACY_EMBEDDED_BROWSER=1
    )
else()
    target_compile_definitions(corei_ww3d2 INTERFACE
        RTS_ENABLE_LEGACY_EMBEDDED_BROWSER=0
    )
endif()

if (MSVC AND NOT IS_VS6_BUILD)
    target_link_libraries(corei_ww3d2 INTERFACE
        comsuppw
        ole32
        windowscodecs
    )
endif()

target_link_libraries(corei_ww3d2 INTERFACE
    core_browserengine
    core_wwlib
    core_wwmath
)

# The native runtime path is deliberately a separate target.  It must never
# inherit the compatibility PCH or the legacy WW3D source list.
if(CMAKE_SIZEOF_VOID_P EQUAL 8 AND WIN32 AND NOT IS_VS6_BUILD)
    add_library(core_ww3d2_native STATIC
        nativew3dline.cpp
        nativew3dline.h
        nativew3dbufferowner.cpp
		nativew3dbufferowner.h
		nativew3dfvf.cpp
		nativew3dbuffers.cpp
		nativew3dbuffercompat.h
		nativew3dsampledtexture.cpp
		nativew3dsampledtexture.h
        nativew3dtextureowner.cpp
        nativew3dtextureowner.h
        nativew3dsorting.cpp
        nativew3dsorting.h
        nativew3dgameclient.cpp
        nativew3dgameclientcommands.cpp
        nativew3d2.cpp
        nativew3d2.h
    )
	target_include_directories(core_ww3d2_native PUBLIC
		"${CMAKE_CURRENT_SOURCE_DIR}"
		"${CMAKE_SOURCE_DIR}/Core/Libraries/Source/WWVegas"
	)
	target_link_libraries(core_ww3d2_native PUBLIC
		core_renderer
		core_task_runtime
		core_utility_no_pch
	)
    target_link_libraries(corei_ww3d2 INTERFACE
        core_ww3d2_native
    )
    set_target_properties(core_ww3d2_native PROPERTIES
        CXX_STANDARD 20
        CXX_STANDARD_REQUIRED YES
        CXX_EXTENSIONS NO
    )
endif()
