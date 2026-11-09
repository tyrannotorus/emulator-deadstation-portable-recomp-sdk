# Nakagawa Recomp's runtime (nakagawa-recomp/src/rt, GPL-3.0-or-later) as a static library, with this SDK's backends in place of
# its SDL3 and Windows ones, and a module's translated game linked against it.
#
#   nakagawa_add_runtime(<target> BACKEND headless|brick)
#   nakagawa_add_game(<exe> RUNTIME <target> GEN <dir from tools/prepare-game.sh>)
#
# The generated C and the runtime's floating point follow the PSP's: no fused multiply-add (-ffp-contract=off), as the Allegrex's
# FPU and VFPU round each product.
set(NK_DIR "${CMAKE_CURRENT_LIST_DIR}/../nakagawa-recomp")
set(NK_SDK_DIR "${CMAKE_CURRENT_LIST_DIR}/..")
find_package(Python3 3.14 REQUIRED COMPONENTS Interpreter)

set(NK_RT_SOURCES
    src/core/nk_font.c src/core/nk_input_profile.c src/core/nk_json.c src/core/nk_platform_posix.c src/core/nk_shared_helpers.c
    src/core/nk_xb.c src/rt/ace.c src/rt/archive_vfs.c src/rt/audio_resample.c src/rt/cpu_lle.c src/rt/debug.c src/rt/domain_mode.c
    src/rt/driver.c src/rt/fbcap.c src/rt/fbcap_policy.c src/rt/flash0_font.c src/rt/flight_recorder.c src/rt/ge.c src/rt/ge_capture.c
    src/rt/guest_interp.c src/rt/guest_printf.c src/rt/h264_ffmpeg.c src/rt/h264_null.c src/rt/hle_audio.c src/rt/hle_audio_codec.c
    src/rt/hle.c src/rt/hle_cache_dma.c src/rt/hle_data.c src/rt/hle_display.c src/rt/hle_font.c src/rt/hle_ge.c src/rt/hle_host.c
    src/rt/hle_input.c src/rt/hle_io.c src/rt/hle_kernel_interrupt.c src/rt/hle_kernel_memory.c src/rt/hle_kernel_module.c
    src/rt/hle_kernel_sync.c src/rt/hle_kernel_thread.c src/rt/hle_kernel_time.c src/rt/hle_kernel_utils.c src/rt/hle_mpeg.c
    src/rt/hle_network.c src/rt/hle_npdrm.c src/rt/hle_power.c src/rt/hle_route.c src/rt/hle_rtc.c src/rt/hle_system.c src/rt/hle_umd.c
    src/rt/hle_utility.c src/rt/iso_public.c src/rt/mp3_bridge.c src/rt/mpeg.c src/rt/nested_frames.c src/rt/osk_overlay.c
    src/rt/osk_overlay_paint.c src/rt/osk_text_entry.c src/rt/osk_win.c src/rt/perf.c src/rt/pgd_unavailable.c src/rt/pgf_public.c src/rt/prx_loader.c
    src/rt/psmf_producer.c src/rt/recomp.c src/rt/replay_checkpoint.c src/rt/savedata.c src/rt/sched.c src/rt/sr_coro.c
    src/rt/stale_code.c src/rt/texrepl.c src/rt/title_config.c src/rt/vfpu_interp.c src/rt/vfpu_tables.c src/rt/videorec.c
    src/rt/watchpoints_file.c)
set(NK_ATRAC_SOURCES
    src/rt/atrac3p_bridge.c src/rt/atrac3p/atrac3p_api.c src/rt/atrac3p/libavcodec/atrac3.c src/rt/atrac3p/libavcodec/atrac3plus.c
    src/rt/atrac3p/libavcodec/atrac3plusdec.c src/rt/atrac3p/libavcodec/atrac3plusdsp.c src/rt/atrac3p/libavcodec/atrac.c
    src/rt/atrac3p/libavcodec/bitstream.c src/rt/atrac3p/libavcodec/fft_float.c src/rt/atrac3p/libavcodec/fft_init_table.c
    src/rt/atrac3p/libavcodec/mdct_float.c src/rt/atrac3p/libavcodec/sinewin.c src/rt/atrac3p/libavutil/float_dsp.c
    src/rt/atrac3p/libavutil/intmath.c src/rt/atrac3p/libavutil/log2_tab.c src/rt/atrac3p/libavutil/mem.c src/rt/atrac3p/libavutil/reverse.c)
set(NK_H264_SOURCES
    libavcodec/allcodecs.c libavcodec/atsc_a53.c libavcodec/avcodec.c libavcodec/avpacket.c libavcodec/bitstream.c
    libavcodec/bitstream_filters.c libavcodec/bsf.c libavcodec/cabac.c libavcodec/codec_desc.c libavcodec/codec_par.c libavcodec/decode.c
    libavcodec/encode.c libavcodec/error_resilience.c libavcodec/golomb.c libavcodec/h2645_parse.c libavcodec/h264_cabac.c
    libavcodec/h264_cavlc.c libavcodec/h264chroma.c libavcodec/h264data.c libavcodec/h264dec.c libavcodec/h264_direct.c
    libavcodec/h264dsp.c libavcodec/h264idct.c libavcodec/h264_loopfilter.c libavcodec/h264_mb.c libavcodec/h264_parse.c
    libavcodec/h264_picture.c libavcodec/h264pred.c libavcodec/h264_ps.c libavcodec/h264qpel.c libavcodec/h264_refs.c libavcodec/h264_sei.c
    libavcodec/h264_slice.c libavcodec/mathtables.c libavcodec/me_cmp.c libavcodec/null_bsf.c libavcodec/options.c libavcodec/profiles.c
    libavcodec/simple_idct.c libavcodec/startcode.c libavcodec/utils.c libavcodec/videodsp.c libavutil/avstring.c libavutil/bprint.c
    libavutil/buffer.c libavutil/channel_layout.c libavutil/dict.c libavutil/display.c libavutil/error.c libavutil/eval.c libavutil/fifo.c
    libavutil/file_open.c libavutil/frame.c libavutil/hwcontext.c libavutil/imgutils.c libavutil/log2_tab.c libavutil/log.c
    libavutil/mathematics.c libavutil/mem.c libavutil/opt.c libavutil/parseutils.c libavutil/pixdesc.c libavutil/random_seed.c
    libavutil/rational.c libavutil/reverse.c libavutil/samplefmt.c libavutil/sha.c libavutil/stereo3d.c libavutil/time.c
    libavutil/timecode.c libavutil/utils.c libavutil/video_enc_params.c nk_h264_api.c)

# Nakagawa's PSP crypto (src/core), for a module's game image made from the player's disc (runtime/image_build.c)
set(NK_PSP_CRYPTO_SOURCES)
foreach(_c crypto keystore builtin_keys aes sha1 ec kirk prx kle inflate container amctrl lzrc npumdimg)
    list(APPEND NK_PSP_CRYPTO_SOURCES "${NK_DIR}/src/core/nk_psp_${_c}.c")
endforeach()
# -fno-pie (with -no-pie at the link): a global is one adrp+ldr away, not a load of its GOT entry first; the generated code reads
# a few at every guest call (the timeslice, the scheduler's switch, the host stack window)
# -fno-stack-protector: the host clang protects the stack by default (__SSP_STRONG__): a canary stored and checked in every function
# with a local array, every translated VFPU body among them, at every guest call
set(NK_COMMON_FLAGS -fno-strict-aliasing -ffp-contract=off -fno-pie -fno-stack-protector)
# libchdr (BSD-3-Clause, Nakagawa's third_party): the disc read straight from the player's CHD (patches/nakagawa/0002)
set(NK_CHDR_SOURCES
    src/libchdr_bitstream.c src/libchdr_cdrom.c src/libchdr_chd.c src/libchdr_codec_avhuff.c src/libchdr_codec_cdfl.c
    src/libchdr_codec_cdlz.c src/libchdr_codec_cdzl.c src/libchdr_codec_cdzs.c src/libchdr_codec_flac.c src/libchdr_codec_huff.c
    src/libchdr_codec_lzma.c src/libchdr_codec_zlib.c src/libchdr_codec_zstd.c src/libchdr_flac.c src/libchdr_huffman.c
    deps/lzma-26.02/src/LzmaDec.c deps/miniz-3.1.2/miniz.c deps/zstd-1.5.7/zstddeclib.c)

function(nakagawa_add_runtime target)
    cmake_parse_arguments(ARG "" "BACKEND" "" ${ARGN})
    if(NOT ARG_BACKEND MATCHES "^(headless|brick)$")
        message(FATAL_ERROR "nakagawa_add_runtime: BACKEND headless or brick")
    endif()
    # the runtime's title configuration: generic (no title manifest), as `make` writes it
    set(_cfg_dir "${CMAKE_CURRENT_BINARY_DIR}/${target}_config")
    file(MAKE_DIRECTORY "${_cfg_dir}")
    execute_process(COMMAND "${Python3_EXECUTABLE}" tools/title_runtime_config.py --output "${_cfg_dir}/sr_title_config.h"
            WORKING_DIRECTORY "${NK_DIR}" RESULT_VARIABLE _res OUTPUT_QUIET)
    if(NOT _res EQUAL 0)
        message(FATAL_ERROR "title_runtime_config.py failed")
    endif()

    list(TRANSFORM NK_RT_SOURCES PREPEND "${NK_DIR}/" OUTPUT_VARIABLE _rt)
    list(TRANSFORM NK_ATRAC_SOURCES PREPEND "${NK_DIR}/" OUTPUT_VARIABLE _atrac)
    list(TRANSFORM NK_H264_SOURCES PREPEND "${NK_DIR}/src/rt/h264_ffmpeg_sub/" OUTPUT_VARIABLE _h264)
    if(ARG_BACKEND STREQUAL "headless")
        # this PC's own SDL3 (the distribution's) for the scheduler's clock, locks and threads; no window, no sound
        find_package(PkgConfig REQUIRED)
        pkg_check_modules(NK_SDL3 REQUIRED IMPORTED_TARGET sdl3)
        list(APPEND _rt "${NK_DIR}/src/rt/gui.c" "${NK_SDK_DIR}/runtime/audio_null.c" "${NK_SDK_DIR}/runtime/gpu_none.c" "${NK_SDK_DIR}/runtime/ge_worker.c")
    else()
        # the Brick: Nakagawa's portable gui.c, with its screen, pad and quit calls taken by runtime/brick/gui_brick.c; SDL3's
        # clock, locks and threads by runtime/brick/sdl3_shim.c (the build PC's SDL3 headers, for the declarations only)
        set(_sdl3 "${CMAKE_BINARY_DIR}/sdl3-headers")
        if(NOT EXISTS "${_sdl3}/SDL3/SDL.h")
            if(NOT EXISTS "/usr/include/SDL3/SDL.h")
                message(FATAL_ERROR "the SDL3 headers (the build PC's /usr/include/SDL3) are needed for the Brick build's declarations")
            endif()
            file(COPY /usr/include/SDL3 DESTINATION "${_sdl3}")
        endif()
        # the GE drawn on the Brick's GPU: Nakagawa's Vulkan backend (gpu_sdl3vk/ge_gpu.c) on the presenter's device
        # (runtime/brick/brickvk.cpp, video.cpp, panel.cpp: fb0's pages)
        list(APPEND _rt "${NK_DIR}/src/rt/gui.c" "${NK_SDK_DIR}/runtime/brick/gui_brick.c" "${NK_SDK_DIR}/runtime/brick/sdl3_shim.c"
                "${NK_SDK_DIR}/runtime/brick/audio_alsa.c" "${NK_SDK_DIR}/runtime/brick/prof.c" "${NK_SDK_DIR}/runtime/brick/ioctl_count.c" "${NK_SDK_DIR}/runtime/brick/host_link.c" "${NK_SDK_DIR}/runtime/brick/affinity.c" "${NK_SDK_DIR}/runtime/image_build.c"
                ${NK_PSP_CRYPTO_SOURCES} "${NK_SDK_DIR}/runtime/ge_worker.c" "${NK_DIR}/src/rt/gpu_sdl3vk/ge_gpu.c" "${NK_DIR}/src/rt/gpu_sdl3vk/tex_scale.c"
                "${NK_DIR}/src/rt/gpu_sdl3vk/ace_hud.c" "${NK_DIR}/third_party/xbrz/xbrz.c")
        set(_vk "${CMAKE_BINARY_DIR}/vulkan-headers")
        if(NOT EXISTS "${_vk}/vulkan/vulkan.hpp")
            file(COPY /usr/include/vulkan /usr/include/vk_video DESTINATION "${_vk}")
        endif()
        find_program(NK_GLSLANG glslangValidator REQUIRED)
        set(_spv)
        foreach(_shader frame.vert frame.frag frame_exact.frag)
            string(REPLACE "." "_" _name ${_shader})
            set(_out "${CMAKE_BINARY_DIR}/brick_shaders/${_name}.h")
            add_custom_command(OUTPUT "${_out}" COMMAND "${NK_GLSLANG}" -V --quiet --vn ${_name} -o "${_out}"
                    "${NK_SDK_DIR}/runtime/brick/shaders/${_shader}" DEPENDS "${NK_SDK_DIR}/runtime/brick/shaders/${_shader}")
            list(APPEND _spv "${_out}")
        endforeach()
        set(_cxx "${NK_SDK_DIR}/runtime/brick/brickvk.cpp" "${NK_SDK_DIR}/runtime/brick/video.cpp" "${NK_SDK_DIR}/runtime/brick/panel.cpp")
        list(APPEND _rt ${_cxx} ${_spv})
        set_source_files_properties(${_cxx} PROPERTIES COMPILE_OPTIONS "-std=c++17;-O2;-include;${NK_SDK_DIR}/runtime/brick/vk.h"
                INCLUDE_DIRECTORIES "${NK_SDK_DIR}/runtime/brick;${CMAKE_BINARY_DIR}/brick_shaders;${_vk};${NK_DIR}/src/rt")
    endif()

    add_library(${target}_h264 OBJECT ${_h264})
    target_include_directories(${target}_h264 PRIVATE "${NK_DIR}/src/rt/h264_ffmpeg_sub" "${NK_DIR}/src/rt" "${NK_DIR}/src/core")
    target_compile_options(${target}_h264 PRIVATE -O2 -w -std=c11 ${NK_COMMON_FLAGS}
            -include "${NK_DIR}/src/rt/h264_ffmpeg_sub/nk_h264_symbols.h")
    target_compile_definitions(${target}_h264 PRIVATE HAVE_AV_CONFIG_H _ISOC99_SOURCE _FILE_OFFSET_BITS=64 _LARGEFILE_SOURCE
            _POSIX_C_SOURCE=200112 _XOPEN_SOURCE=600)

    set(_chdr_dir "${NK_DIR}/third_party/libchdr")
    list(TRANSFORM NK_CHDR_SOURCES PREPEND "${_chdr_dir}/" OUTPUT_VARIABLE _chdr)
    add_library(${target}_chdr OBJECT ${_chdr})
    target_include_directories(${target}_chdr PRIVATE "${_chdr_dir}/include" "${_chdr_dir}/deps/lzma-26.02/include"
            "${_chdr_dir}/deps/miniz-3.1.2" "${_chdr_dir}/deps/zstd-1.5.7")
    target_compile_options(${target}_chdr PRIVATE -std=c99 -O2 -w)
    target_compile_definitions(${target}_chdr PRIVATE MINIZ_NO_ARCHIVE_APIS MINIZ_NO_DEFLATE_APIS MINIZ_NO_STDIO MINIZ_NO_TIME)

    add_library(${target} STATIC ${_rt} ${_atrac} $<TARGET_OBJECTS:${target}_h264> $<TARGET_OBJECTS:${target}_chdr>)
    target_include_directories(${target} PUBLIC "${NK_DIR}/src/rt" "${NK_DIR}/src/core" "${_cfg_dir}"
            PRIVATE "${NK_DIR}/src" "${NK_DIR}/src/rt/atrac3p" "${NK_DIR}/src/rt/atrac3p/libavcodec" "${NK_DIR}/third_party"
                    "${_chdr_dir}/include")
    # line tables (no change to the code): the profiler's samples resolve to source lines (addr2line -i)
    # each function in a section of its own: the link can place the hot ones together (NK_SYMBOL_ORDER)
    target_compile_options(${target} PRIVATE -O2 -w ${NK_COMMON_FLAGS} -gline-tables-only -ffunction-sections)
    target_compile_definitions(${target} PUBLIC _GNU_SOURCE SR_FLIGHT_RECORDER_LINKED PERF_AOT_INSTRUCTIONS=0 SR_H264_FFMPEG NK_ISO_CHD SR_GE_NATIVE SR_FPU_NATIVE
            SR_CPU_NATIVE SR_MEM_FIXED)
    target_link_libraries(${target} PUBLIC m pthread dl)
    target_link_options(${target} INTERFACE -no-pie)
    if(ARG_BACKEND STREQUAL "headless")
        target_link_libraries(${target} PUBLIC PkgConfig::NK_SDL3)
    else()
        target_include_directories(${target} SYSTEM PUBLIC "${CMAKE_BINARY_DIR}/sdl3-headers" "${CMAKE_BINARY_DIR}/vulkan-headers")
        # the Brick's own Vulkan loader (adb pull /usr/lib/libvulkan.so.1 into NK_BRICK_LIBS): ge_gpu.c calls Vulkan directly
        if(NOT EXISTS "${NK_BRICK_LIBS}/libvulkan.so")
            message(FATAL_ERROR "NK_BRICK_LIBS: a folder with the Brick's libvulkan.so (adb pull /usr/lib/libvulkan.so.1)")
        endif()
        target_link_libraries(${target} PUBLIC "${NK_BRICK_LIBS}/libvulkan.so" asound stdc++)
        # (dev) runtime/brick/ioctl_count.c: the driver's ioctl calls come to the executable's (DSP_IOCTLCOUNT=1)
        target_link_options(${target} INTERFACE -Wl,--export-dynamic-symbol=ioctl)
        target_link_options(${target} INTERFACE -Wl,--wrap=main -Wl,--wrap=gui_on -Wl,--wrap=gui_present -Wl,--wrap=gui_buttons -Wl,--wrap=gui_analog
                -Wl,--wrap=gui_pad_present -Wl,--wrap=gui_quit_requested -Wl,--wrap=gui_stop_requested)
    endif()
endfunction()

function(nakagawa_add_game exe)
    # SOURCES: the module's own C beside the translation (its settings, its changes to the game)
    cmake_parse_arguments(ARG "" "RUNTIME;GEN" "SOURCES" ${ARGN})
    file(GLOB _chunks "${ARG_GEN}/game_[0-9]*.c")
    if(NOT _chunks)
        message(FATAL_ERROR "nakagawa_add_game: no generated chunks in ${ARG_GEN} (tools/prepare-game.sh)")
    endif()
    add_executable(${exe} "${ARG_GEN}/game.c" ${_chunks} "${NK_SDK_DIR}/runtime/hle_sdk.c" ${ARG_SOURCES})
    # (the runtime's internal headers: its own libavutil for the ATRAC decoder's, never the system's)
    set_source_files_properties("${NK_SDK_DIR}/runtime/hle_sdk.c" PROPERTIES INCLUDE_DIRECTORIES "${NK_DIR}/src/rt/atrac3p;${NK_DIR}/src/rt;${NK_DIR}/src/core")
    target_include_directories(${exe} PRIVATE "${ARG_GEN}")
    # the translated chunks without a frame pointer: one more register for their guest registers in C locals (the runtime keeps
    # frame pointers: the profiler and __builtin_frame_address)
    target_compile_options(${exe} PRIVATE -O2 -w ${NK_COMMON_FLAGS} -fomit-frame-pointer -ffunction-sections)
    target_link_libraries(${exe} PRIVATE ${ARG_RUNTIME})
    # NK_SYMBOL_ORDER (a file of function names, hottest first, from a profile of the game on the Brick; kept out of the repos
    # with the rest of a title's derived data): those functions linked together at the start of the code, so the game's hot code
    # sits in a few MB instead of across the whole translation (the A53's instruction cache and TLBs)
    if(NK_SYMBOL_ORDER)
        target_link_options(${exe} PRIVATE -Wl,--symbol-ordering-file=${NK_SYMBOL_ORDER} -Wl,--no-warn-symbol-ordering)
    endif()
endfunction()
