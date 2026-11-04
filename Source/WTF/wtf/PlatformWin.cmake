list(APPEND WTF_SOURCES
    generic/WorkQueueGeneric.cpp

    text/win/StringWin.cpp
    text/win/TextBreakIteratorInternalICUWin.cpp

    win/CPUTimeWin.cpp
    win/DbgHelperWin.cpp
    win/FileHandleWin.cpp
    win/FileSystemWin.cpp
    win/LanguageWin.cpp
    win/LoggingWin.cpp
    win/MainThreadWin.cpp
    win/MappedFileDataWin.cpp
    win/MemoryFootprintWin.cpp
    win/MemoryPressureHandlerWin.cpp
    win/OSAllocatorWin.cpp
    win/PathWalker.cpp
    win/RunLoopWin.cpp
    win/SignalsWin.cpp
    win/ThreadingWin.cpp
    win/WTFCRTDebug.cpp
    win/Win32Handle.cpp
)

list(APPEND WTF_PUBLIC_HEADERS
    PlatformEnableWin.h

    text/win/WCharStringExtras.h

    win/DbgHelperWin.h
    win/GDIObject.h
    win/PathWalker.h
    win/SoftLinking.h
    win/WTFCRTDebug.h
    win/Win32Handle.h
)

list(APPEND WTF_LIBRARIES
    DbgHelp
    shlwapi
    synchronization
    winmm
)

# MyBrowser: the GStreamer media backend uses WTF's GLib smart pointers and helpers.
# Only those are built; the run loop stays the native Windows one (no RunLoopGLib).
if (USE_GSTREAMER)
    list(APPEND WTF_PUBLIC_HEADERS
        glib/GLibExtras.h
        glib/GMallocString.h
        glib/GRefPtr.h
        glib/GSpanExtras.h
        glib/GThreadSafeWeakPtr.h
        glib/GTypedefs.h
        glib/GUniquePtr.h
        glib/GWeakPtr.h
        glib/RunLoopSourcePriority.h
        glib/WTFGType.h
    )
    list(APPEND WTF_SOURCES
        glib/GMallocString.cpp
        glib/GRefPtr.cpp
        glib/GSpanExtras.cpp
    )
    list(APPEND WTF_LIBRARIES
        GLib::Gio
    )
endif ()
