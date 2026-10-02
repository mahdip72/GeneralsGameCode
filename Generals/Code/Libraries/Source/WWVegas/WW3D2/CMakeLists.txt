set(WW3D2_SRC
    #aabtree.cpp
    #aabtree.h
    aabtreebuilder.cpp
    aabtreebuilder.h
    #agg_def.cpp
    #agg_def.h
    #animatedsoundmgr.cpp
    #animatedsoundmgr.h
    animobj.cpp
    animobj.h
    assetmgr.cpp
    assetmgr.h
    #assetstatus.cpp
    #assetstatus.h
    #bitmaphandler.cpp
    #bitmaphandler.h
    #bmp2d.cpp
    #bmp2d.h
    boxrobj.cpp
    boxrobj.h
    #bwrender.cpp
    #bwrender.h
    camera.cpp
    camera.h
    #classid.h
    #collect.cpp
    #collect.h
    #colorspace.h
    #coltest.cpp
    #coltest.h
    #coltype.h
    #composite.cpp
    #composite.h
    dazzle.cpp
    dazzle.h
    ddsfile.cpp
    ddsfile.h
    decalmsh.cpp
    decalmsh.h
    #decalsys.cpp
    #decalsys.h
    #distlod.cpp
    #distlod.h
    #dllist.h
    #dynamesh.cpp
    #dynamesh.h
    #font3d.cpp
    #font3d.h
    #formconv.cpp
    #formconv.h
    #FramGrab.cpp
    #framgrab.h
    #hanim.cpp
    #hanim.h
    hanimmgr.cpp
    hanimmgr.h
    #hcanim.cpp
    #hcanim.h
    hlod.cpp
    hlod.h
    #hmdldef.cpp
    #hmdldef.h
    hmorphanim.cpp
    hmorphanim.h
    hrawanim.cpp
    hrawanim.h
    #htree.cpp
    #htree.h
    htreemgr.cpp
    htreemgr.h
    #intersec.cpp
    #intersec.h
    #intersec.inl
    #inttest.h
    #layer.cpp
    #layer.h
    light.cpp
    light.h
    lightenvironment.cpp
    lightenvironment.h
    #line3d.cpp
    #line3d.h
    mapper.cpp
    mapper.h
    #matinfo.cpp
    #matinfo.h
    #matpass.cpp
    #matpass.h
    matrixmapper.cpp
    matrixmapper.h
    mesh.cpp
    mesh.h
    meshbuild.cpp
    meshbuild.h
    #meshdam.cpp
    #meshdam.h
    meshgeometry.cpp
    meshgeometry.h
    meshmatdesc.cpp
    meshmatdesc.h
    meshmdl.cpp
    meshmdl.h
    meshmdlio.cpp
    #metalmap.cpp
    #metalmap.h
    #missingtexture.cpp
    #missingtexture.h
    motchan.cpp
    motchan.h
    nullrobj.cpp
    nullrobj.h
    part_buf.cpp
    part_buf.h
    part_emt.cpp
    part_emt.h
    part_ldr.cpp
    part_ldr.h
    #pivot.cpp
    #pivot.h
    #pointgr.cpp
    #pointgr.h
    #polyinfo.cpp
    #polyinfo.h
    #predlod.cpp
    #predlod.h
    #prim_anim.cpp
    #prim_anim.h
    #projector.cpp
    #projector.h
    #proto.cpp
    #proto.h
    #proxy.h
    #rddesc.h
    render2d.cpp
    render2d.h
    #render2dsentence.cpp
    #render2dsentence.h
    #renderobjectrecycler.cpp
    #renderobjectrecycler.h
    #rendobj.cpp
    #rendobj.h
    rinfo.cpp
    rinfo.h
    #ringobj.cpp
    #ringobj.h
    #robjlist.h
    scene.cpp
    scene.h
    #segline.cpp
    #segline.h
    #seglinerenderer.cpp
    #seglinerenderer.h
    shader.cpp
    shader.h
    #shattersystem.cpp
    #shattersystem.h
    #snappts.cpp
    #snapPts.h
#    sortingrenderer.cpp
#    sortingrenderer.h
    #soundlibrarybridge.h
    #soundrobj.cpp
    #soundrobj.h
    #sphereobj.cpp
    #sphereobj.h
    #static_sort_list.cpp
    #static_sort_list.h
    #statistics.cpp
    #statistics.h
    #streak.cpp
    #streak.h
    #streakRender.cpp
    #streakRender.h
    #stripoptimizer.cpp
    #stripoptimizer.h
    #surfaceclass.cpp
    #surfaceclass.h
    #texproject.cpp
    #texproject.h
    #textdraw.cpp
    #textdraw.h
    #texture.cpp
    #texture.h
    #texturefilter.cpp
    #texturefilter.h
    #textureloader.cpp
    #textureloader.h
    #texturethumbnail.cpp
    #texturethumbnail.h
    vertmaterial.cpp
    vertmaterial.h
    #visrasterizer.cpp
    #visrasterizer.h
    #w3d_dep.cpp
    #w3d_dep.h
    w3d_file.h
    #w3d_obsolete.h
    #w3d_util.cpp
    #w3d_util.h
    #w3derr.h
    #w3dexclusionlist.cpp
    #w3dexclusionlist.h
#    ww3d.cpp
#    ww3d.h
    #ww3dformat.cpp
    #ww3dformat.h
    #ww3dids.h
    #ww3dtrig.h
)

add_library(g_ww3d2 STATIC)
set_target_properties(g_ww3d2 PROPERTIES OUTPUT_NAME ww3d2)

target_sources(g_ww3d2 PRIVATE ${WW3D2_SRC})

# Mesh loading is CPU-side file parsing and does not require the renderer PCH.
# Keep the native translation unit independent from the compatibility PCH.
if(CMAKE_SIZEOF_VOID_P EQUAL 8 AND WIN32 AND NOT IS_VS6_BUILD)
    set_source_files_properties(
        "${CMAKE_CURRENT_SOURCE_DIR}/meshmdlio.cpp"
        PROPERTIES SKIP_PRECOMPILE_HEADERS ON)
endif()

# Line3D's native implementation consumes this title's ShaderClass ABI. Keep
# it in the product target so the two titles do not share title headers through
# core_ww3d2_native.
if(CMAKE_SIZEOF_VOID_P EQUAL 8 AND WIN32 AND NOT IS_VS6_BUILD)
    set(_g_native_line3d
        "${CMAKE_SOURCE_DIR}/Core/Libraries/Source/WWVegas/WW3D2/nativeLine3d.cpp")
    target_sources(g_ww3d2 PRIVATE ${_g_native_line3d})
    set(_g_native_title_renderer_sources
        "${CMAKE_SOURCE_DIR}/Core/Libraries/Source/WWVegas/WW3D2/nativew3dmeshrenderer.cpp"
        "${CMAKE_SOURCE_DIR}/Core/Libraries/Source/WWVegas/WW3D2/nativew3dpolygonrenderer.cpp"
        "${CMAKE_SOURCE_DIR}/Core/Libraries/Source/WWVegas/WW3D2/nativew3drendererdebugger.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/RenderGameClientNativeTitle.cpp")
    target_sources(g_ww3d2 PRIVATE ${_g_native_title_renderer_sources})
    set_source_files_properties(${_g_native_line3d}
        PROPERTIES SKIP_PRECOMPILE_HEADERS ON)
    set_source_files_properties(${_g_native_title_renderer_sources}
        PROPERTIES SKIP_PRECOMPILE_HEADERS ON)
    target_include_directories(g_ww3d2 PRIVATE
        "${CMAKE_SOURCE_DIR}/Core/Libraries/Source/WWVegas/WW3D2"
    )
endif()

target_include_directories(g_ww3d2 PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}"
)

target_compile_definitions(g_ww3d2 PRIVATE
    $<$<BOOL:${IS_VS6_BUILD}>:WINVER=0x0500>
)

rts_configure_title_ww3d2(g_ww3d2 GENERALS)

target_link_libraries(g_ww3d2 PUBLIC
	core_config
	core_renderer
)

target_link_libraries(g_ww3d2 PRIVATE
	core_wwdebug
    corei_ww3d2
    g_wwcommon
    gi_always
)
