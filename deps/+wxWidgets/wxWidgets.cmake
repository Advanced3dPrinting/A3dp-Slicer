set(_wx_toolkit "")
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    option(DEP_WX_GTK3 "Build wxWidgets for GTK3 instead of GTK2" OFF)

    set(_gtk_ver 2)
    if (DEP_WX_GTK3)
        set(_gtk_ver 3)
    endif ()
    set(_wx_toolkit "-DwxBUILD_TOOLKIT=gtk${_gtk_ver}")
endif()
if (MSVC)
    set(_wx_edge "-DwxUSE_WEBVIEW_EDGE=ON")
else ()
    set(_wx_edge "-DwxUSE_WEBVIEW_EDGE=OFF")
endif ()
set(_unicode_utf8 OFF)
if (UNIX AND NOT APPLE) # wxWidgets will not use char as the underlying type for wxString unless its forced to.
    set (_unicode_utf8 ON)
endif()

add_cmake_project(wxWidgets
    URL https://github.com/prusa3d/wxWidgets/archive/78aa2dc0ea7ce99dc19adc1140f74c3e2e3f3a26.zip
    URL_HASH SHA256=94b7d972373503e380e5a8b0ca63b1ccb956da4006402298dd89a0c5c7041b1e
    # The pinned fork's find modules hardcode webkit2gtk-4.0 (libsoup2), which
    # is EOL and absent from modern distros (Ubuntu 22.10+/Debian 12/Arch).
    # Retarget them to webkit2gtk-4.1 (libsoup3) - the webkit2 C API and the
    # include tree layout (webkitgtk-4.1) are identical. webview_webkit2.cpp
    # additionally uses soup2-only transport status codes (removed in libsoup3)
    # - guarded by webview_webkit2_soup3_compat.patch.
    PATCH_COMMAND /bin/sh -c
        "sed -i 's/WEBKIT2_VERSION 4\\.0/WEBKIT2_VERSION 4.1/' build/cmake/modules/FindWEBKIT2.cmake && sed -i 's/soup-2\\.4/soup-3.0/g' build/cmake/modules/FindLIBSOUP.cmake"
    COMMAND ${PATCH_CMD} ${CMAKE_CURRENT_LIST_DIR}/webview_webkit2_soup3_compat.patch
    CMAKE_ARGS
        "-DCMAKE_DEBUG_POSTFIX:STRING="
        -DwxBUILD_PRECOMP=ON
        ${_wx_toolkit}
        -DwxUSE_MEDIACTRL=OFF
        ${_wx_edge}
        -DwxUSE_WEBVIEW_IE=OFF
        -DwxUSE_SCINTILLA=ON
        -DwxUSE_DETECT_SM=OFF
        -DwxUSE_UNICODE=ON
        -DwxUSE_UNICODE_UTF8=${_unicode_utf8}
        -DwxUSE_OPENGL=ON
        -DwxUSE_WEBVIEW=ON
        -DwxUSE_WEBVIEW_WEBKIT=ON
        -DwxUSE_LIBPNG=sys
        -DwxUSE_ZLIB=sys
        -DwxUSE_NANOSVG=sys
        -DwxUSE_NANOSVG_EXTERNAL=ON
        -DwxUSE_REGEX=OFF
        -DwxUSE_LIBXPM=builtin
        -DwxUSE_LIBJPEG=sys
        -DwxUSE_LIBTIFF=OFF
        -DwxUSE_EXPAT=sys
        -DwxUSE_LIBSDL=OFF
        -DwxUSE_STC=ON
        -DwxUSE_XTEST=OFF
        -DwxUSE_GLCANVAS_EGL=OFF
        -DwxUSE_WEBREQUEST=OFF
)

set(DEP_wxWidgets_DEPENDS ZLIB PNG EXPAT JPEG NanoSVG)
