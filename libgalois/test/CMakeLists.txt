function(add_test_unit name)
  set(no_value_options NOT_QUICK)
  set(multi_value_options REQUIRES)
  set(multi_value_options REQUIRES LINK_LIBRARIES)
  cmake_parse_arguments(X "${no_value_options}" "" "${multi_value_options}" ${ARGN})

  foreach(required ${X_REQUIRES})
    if(${${required}} MATCHES "TRUE")
    else()
      message(STATUS "NOT compiling ${name} (missing: ${required})")
      return()
    endif()
  endforeach()

  set(test_name ${name}-test)

  add_executable(${test_name} ${name}.cpp)
  target_link_libraries(${test_name} katana_galois)
  if(X_LINK_LIBRARIES)
    target_link_libraries(${test_name} ${X_LINK_LIBRARIES})
  endif()

  list(APPEND command_line "$<TARGET_FILE:${test_name}>")
  list(APPEND command_line ${X_UNPARSED_ARGUMENTS})

  add_test(NAME ${test_name} COMMAND ${command_line})

  set_tests_properties(${test_name}
    PROPERTIES
      ENVIRONMENT KATANA_DO_NOT_BIND_THREADS=1)

  # Allow parallel tests
  if(NOT X_NOT_QUICK)
    set_tests_properties(${test_name}
      PROPERTIES
        LABELS quick
      )
  endif()
endfunction()

add_test_unit(acquire)
add_test_unit(bandwidth)
add_test_unit(barriers 1024 2)
add_test_unit(empty-member-lcgraph)
add_test_unit(flatmap)
add_test_unit(floating-point-errors)
add_test_unit(foreach)
add_test_unit(foreach-bench LINK_LIBRARIES benchmark::benchmark)
add_test_unit(forward-declare-graph)
add_test_unit(gcollections)
add_test_unit(graph)
add_test_unit(graph-compile)
add_test_unit(gslist)
add_test_unit(hwtopo)
add_test_unit(lock)
add_test_unit(loop-overhead REQUIRES OPENMP_FOUND)
add_test_unit(mem)
add_test_unit(morph-graph)
add_test_unit(morph-graph-removal)
add_test_unit(move)
add_test_unit(offset)
add_test_unit(oneach)
add_test_unit(papi 2)
add_test_unit(range)
add_test_unit(pc)
add_test_unit(property-file-graph)
add_test_unit(graph-predicates "${BASEINPUT}/propertygraphs/rmat10" LINK_LIBRARIES LLVMSupport)
#TODO(emcginnis): when the default input graphs are latest_storage_format_version instead of v1, need to change these paths
add_test_unit(storage-format-version-v1-v2-entity-type-ids "${BASEINPUT}/propertygraphs/ldbc_003" LINK_LIBRARIES LLVMSupport)
add_test_unit(storage-format-version-v2-v3-entity-type-ids "${BASEINPUT}/propertygraphs/ldbc_003_storage_format_version_2" LINK_LIBRARIES LLVMSupport)
add_test_unit(storage-format-version-v3-optional-topologies "${BASEINPUT}/propertygraphs/ldbc_003" LINK_LIBRARIES LLVMSupport)
add_test_unit(property-graph)
add_test_unit(property-graph-diff)
add_test_unit(property-graph-bench NOT_QUICK LINK_LIBRARIES benchmark::benchmark)
add_test_unit(property-graph-in-memory-props)
add_test_unit(property-graph-topology)
add_test_unit(property-graph-optional-topology-generation "${BASEINPUT}/propertygraphs/ldbc_003" LINK_LIBRARIES LLVMSupport)
add_test_unit(property-index)
add_test_unit(property-view)
add_test_unit(reduction)
add_test_unit(sort)
add_test_unit(static)
add_test_unit(traits)
add_test_unit(extra-traits)
add_test_unit(two-level-iterator)
add_test_unit(verify-triangle-counting)
add_test_unit(wakeup-overhead LINK_LIBRARIES LLVMSupport)
add_test_unit(worklists-compile)
add_test_unit(projection "${BASEINPUT}/propertygraphs/ldbc_003" City,Comment,Company,Continent,Country,Forum HAS_CREATOR,HAS_INTEREST,HAS_MEMBER,HAS_MODERATOR,HAS_TAG,HAS_TYPE,IS_PART_OF,IS_SUBCLASS_OF,KNOWS,LIKES LINK_LIBRARIES LLVMSupport)
