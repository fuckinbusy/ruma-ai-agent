# Architecture invariants checked at configure time.

# Every target `target` links, directly or through other targets (LINK_LIBRARIES and
# INTERFACE_LINK_LIBRARIES, recursively), into the variable named by `out`.
function(wa_collect_links target out)
    set(seen "")
    set(todo "${target}")
    while(todo)
        list(POP_FRONT todo current)
        if(NOT TARGET "${current}" OR current IN_LIST seen)
            continue()
        endif()
        list(APPEND seen "${current}")
        foreach(property LINK_LIBRARIES INTERFACE_LINK_LIBRARIES)
            get_target_property(links "${current}" ${property})
            if(links)
                foreach(link IN LISTS links)
                    # $<LINK_ONLY:x> and similar wrappers: the target inside.
                    string(REGEX REPLACE "^\\$<[A-Z_]+:(.*)>$" "\\1" link "${link}")
                    list(APPEND todo "${link}")
                endforeach()
            endif()
        endforeach()
    endwhile()
    list(REMOVE_ITEM seen "${target}")
    set(${out} "${seen}" PARENT_SCOPE)
endfunction()

# Fails the configure step when `target` reaches any of the targets after it.
function(wa_forbid_link target)
    wa_collect_links(${target} reached)
    foreach(forbidden IN LISTS ARGN)
        if(forbidden IN_LIST reached)
            message(FATAL_ERROR "${target} must not link ${forbidden} (01-ARCHITECTURE § Invariants); it reaches: ${reached}")
        endif()
    endforeach()
endfunction()
