#
# 86Box-Next: LatestVersions/ in the repository root (not tracked by git)
# always holds an archive of the newest 86Box-Next2 and one of the newest
# isp-server.  Run after every build: each executable is stripped, packed
# with COPYING in a folder of its own name, and its archive replaces the
# previous one (written beside it, then renamed, so a reader never sees half
# an archive).  BUILD.txt says which build and commits they are.
#
#   cmake -DOUT=<LatestVersions> -DSTAGE=<scratch dir> -DSRC=<repository root>
#         -DEMU=<86Box-Next2 exe> -DISP=<isp-server exe> -DCOPYING=<file>
#         -DHEADER=<next_build.h> -DSTRIP=<strip, may be empty>
#         -DSUFFIX=<e.g. linux-x86_64> -DFORMAT=gnutar|zip
#         -P LatestVersions.cmake
#

if(FORMAT STREQUAL "zip")
    set(ext ".zip")
    set(flags "cf")
else()
    set(ext ".tar.gz")
    set(flags "czf")
endif()

file(MAKE_DIRECTORY "${OUT}")

function(pack name exe)
    get_filename_component(exe_name "${exe}" NAME)
    set(dir "${STAGE}/${name}")
    file(REMOVE_RECURSE "${dir}")
    file(MAKE_DIRECTORY "${dir}")
    if(STRIP)
        execute_process(COMMAND "${STRIP}" -o "${dir}/${exe_name}" "${exe}" RESULT_VARIABLE res)
        if(NOT res EQUAL 0)
            message(FATAL_ERROR "LatestVersions: could not strip ${exe}")
        endif()
    else()
        file(COPY "${exe}" DESTINATION "${dir}")
    endif()
    file(COPY "${COPYING}" DESTINATION "${dir}")

    set(archive "${OUT}/${name}-${SUFFIX}${ext}")
    execute_process(COMMAND "${CMAKE_COMMAND}" -E tar ${flags} "${archive}.part" --format=${FORMAT} "${name}"
                    WORKING_DIRECTORY "${STAGE}" RESULT_VARIABLE res)
    if(NOT res EQUAL 0)
        message(FATAL_ERROR "LatestVersions: could not write ${archive}")
    endif()
    file(RENAME "${archive}.part" "${archive}")
    file(REMOVE_RECURSE "${dir}")
endfunction()

pack(86Box-Next2 "${EMU}")
pack(isp-server "${ISP}")

# Which build: the number and release from the generated header, the commits
# from git (-dirty when the tree had changes).
file(READ "${HEADER}" header)
string(REGEX MATCH "NEXT_BUILD_NUMBER ([0-9]+)" _ "${header}")
set(build "build ${CMAKE_MATCH_1}")
string(REGEX MATCH "NEXT_RELEASE \"([^\"]*)\"" _ "${header}")
if(CMAKE_MATCH_1)
    set(build "Release ${CMAKE_MATCH_1}, ${build}")
endif()

find_package(Git QUIET)
function(describe dir var)
    set(rev "unknown commit")
    if(GIT_FOUND)
        # --exclude=*: the commit alone, not the distance from upstream's tags.
        execute_process(COMMAND "${GIT_EXECUTABLE}" -C "${dir}" describe --always --dirty --abbrev=9 --exclude=*
                        OUTPUT_VARIABLE out OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET RESULT_VARIABLE res)
        if(res EQUAL 0)
            set(rev "${out}")
        endif()
    endif()
    set(${var} "${rev}" PARENT_SCOPE)
endfunction()
describe("${SRC}" emu_rev)
describe("${SRC}/isp-server" isp_rev)

string(TIMESTAMP now "%Y-%m-%d %H:%M")
file(WRITE "${OUT}/BUILD.txt"
     "86Box-Next2 ${build} (${emu_rev})\n"
     "isp-server ${isp_rev}\n"
     "Built ${now}\n")
message(STATUS "LatestVersions: 86Box-Next2 ${build}, isp-server ${isp_rev} (${SUFFIX})")
