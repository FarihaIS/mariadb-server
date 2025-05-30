# Copyright (c) 2009, 2013, Oracle and/or its affiliates. All rights reserved.
# Copyright (c) 2012, 2018, MariaDB Corporation
# 
# This program is free software; you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation; version 2 of the License.
# 
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License for more details.
# 
# You should have received a copy of the GNU General Public License
# along with this program; if not, write to the Free Software
# Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1335 USA

INSTALL_MYSQL_TEST("." "")

IF(NOT ${CMAKE_SOURCE_DIR} STREQUAL ${CMAKE_BINARY_DIR})
  # Enable running mtr from build directory
  CONFIGURE_FILE(
    ${CMAKE_CURRENT_SOURCE_DIR}/mtr.out-of-source
    ${CMAKE_CURRENT_BINARY_DIR}/mariadb-test-run.pl
    @ONLY
  )
  CONFIGURE_FILE(
    ${CMAKE_CURRENT_SOURCE_DIR}/mariadb-stress-test.pl
    ${CMAKE_CURRENT_BINARY_DIR}/mariadb-stress-test.pl
    @ONLY
  )
  IF(WIN32)
    CONFIGURE_FILE(
      ${CMAKE_CURRENT_SOURCE_DIR}/mtr.out-of-source
      ${CMAKE_CURRENT_BINARY_DIR}/mysql-test-run.pl
      @ONLY)
  ENDIF()
  SET(out_of_source_build TRUE)
ELSEIF(WIN32)
  CONFIGURE_FILE(
    ${CMAKE_CURRENT_SOURCE_DIR}/mariadb-test-run.pl
    ${CMAKE_CURRENT_BINARY_DIR}/mysql-test-run.pl
    COPYONLY)
    CONFIGURE_FILE(
    ${CMAKE_CURRENT_SOURCE_DIR}/mariadb-stress-test.pl
    ${CMAKE_CURRENT_BINARY_DIR}/mysql-stress-test.pl
    COPYONLY)
ENDIF()

IF(UNIX)
  EXECUTE_PROCESS(
  COMMAND chmod +x  mariadb-test-run.pl
  WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
  FOREACH (lnk mysql-test-run mtr mysql-test-run.pl mariadb-test-run)
    EXECUTE_PROCESS(
     COMMAND ${CMAKE_COMMAND} -E create_symlink
       ./mariadb-test-run.pl ${lnk}
     WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})

    IF(INSTALL_MYSQLTESTDIR)
      INSTALL(FILES ${CMAKE_CURRENT_BINARY_DIR}/${lnk}
        DESTINATION ${INSTALL_MYSQLTESTDIR}
        COMPONENT Test)
    ENDIF()
  ENDFOREACH()
  EXECUTE_PROCESS(
    COMMAND chmod +x mariadb-stress-test.pl
    COMMAND ${CMAKE_COMMAND} -E create_symlink
    ./mariadb-stress-test.pl mysql-stress-test.pl
    WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
  )
ELSE()
  IF(INSTALL_MYSQLTESTDIR)
    INSTALL(FILES ${CMAKE_CURRENT_SOURCE_DIR}/mariadb-test-run.pl
      RENAME mysql-test-run.pl
      DESTINATION ${INSTALL_MYSQLTESTDIR}
      COMPONENT Test)
    INSTALL(FILES ${CMAKE_CURRENT_SOURCE_DIR}/mariadb-stress-test.pl
      RENAME mysql-stress-test.pl
      DESTINATION ${INSTALL_MYSQLTESTDIR}
      COMPONENT Test)
  ENDIF()
ENDIF()

IF(CMAKE_GENERATOR MATCHES "Visual Studio")
  SET(SETCONFIG_COMMAND set MTR_VS_CONFIG=${CMAKE_CFG_INTDIR})
ELSEIF(CMAKE_GENERATOR MATCHES "Xcode")
  SET(SETCONFIG_COMMAND export MTR_VS_CONFIG=${CMAKE_CFG_INTDIR})
ELSE()
  SET(SETCONFIG_COMMAND echo Running tests)
ENDIF()
IF(CYGWIN)
  # On cygwin, pretend to be "Unix" system
  SET(SETOS_COMMAND export MTR_CYGWIN_IS_UNIX=1)
ELSE()
  SET(SETOS_COMMAND echo OS=${CMAKE_SYSTEM_NAME})
ENDIF()



SET(EXP --experimental=collections/default.experimental)
IF(WIN32)
 SET(SET_ENV set)
ELSE()
 SET(SET_ENV export)
ENDIF()


SET(MTR_FORCE perl ./mysql-test-run.pl --force)
IF(EXISTS ${CMAKE_SOURCE_DIR}/mysql-test/suite/nist)
  SET(TEST_NIST ${MTR_FORCE} --comment=nist suite=nist ${EXP} && 
    ${MTR_FORCE} --comment=nist --force --suite=nist+ps ${EXP})
ELSE()
  SET(TEST_NIST echo "NIST tests not found")
ENDIF()

IF(WITH_EMBEDDED_SERVER)
 SET(TEST_EMBEDDED  ${MTR_FORCE} --comment=embedded  --timer  --embedded-server 
   --skip-rpl ${EXP})
ELSE()
 SET(TEST_EMBEDDED echo "Can not test embedded, not compiled in")
ENDIF()

SET(TEST_BT_START
  COMMAND ${SETCONFIG_COMMAND}
  COMMAND ${SETOS_COMMAND}
  COMMAND ${SET_ENV} MTR_BUILD_THREAD=auto
)

ADD_CUSTOM_TARGET(test-force
  ${TEST_BT_START}
  COMMAND ${MTR_FORCE}
)

ADD_CUSTOM_TARGET(test-bt
  ${TEST_BT_START}
  COMMAND ${MTR_FORCE} --comment=normal --timer  --report-features ${EXP}
  COMMAND ${MTR_FORCE} --comment=ps --timer	 --ps-protocol ${EXP}
  COMMAND ${MTR_FORCE} --comment=funcs1+ps --ps-protocol --reorder --suite=funcs_1 ${EXP}
  COMMAND ${MTR_FORCE} --comment=funcs2 --suite=funcs_2 ${EXP}
  COMMAND ${MTR_FORCE} --comment=partitions --suite=parts ${EXP}
  COMMAND ${MTR_FORCE} --comment=stress --suite=stress ${EXP}
  COMMAND ${MTR_FORCE} --force --comment=jp --suite=jp ${EXP}
  COMMAND ${TEST_NIST}
  COMMAND ${TEST_EMBEDDED}
)

ADD_CUSTOM_TARGET(test-bt-fast
  ${TEST_BT_START}
  COMMAND ${MTR_FORCE}  --comment=ps  --timer  --ps-protocol --report-features ${EXP}
  COMMAND ${MTR_FORCE} --comment=stress --suite=stress ${EXP}
)

ADD_CUSTOM_TARGET(test-bt-debug
  ${TEST_BT_START}
  COMMAND ${MTR_FORCE} --comment=debug --timer  --skip-rpl --report-features ${EXP}
)

# Process .in files with includes in collections/

MACRO(PROCESS_COLLECTION_INCLUDE collin collection)
  FILE(STRINGS ${collin} inlines)
  FOREACH(line ${inlines})
    IF(${line} MATCHES "#include .*")
      STRING(REPLACE "#include " "collections/" incfile ${line})
      FILE(READ ${incfile} contents)
      FILE(APPEND ${collection} "${contents}")
    ELSE()
      FILE(APPEND ${collection} "${line}\n")
    ENDIF()
  ENDFOREACH()
ENDMACRO()

#FILE(GLOB infiles "collections/*.in")
#FOREACH(collin ${infiles})
#  STRING(REPLACE ".in" "" collection ${collin})
#  STRING(REPLACE ".in" ".done" colldone ${collin})
#  # Only generate file once
#  IF(NOT EXISTS ${colldone})
#    PROCESS_COLLECTION_INCLUDE(${collin} ${collection})
#    FILE(APPEND ${colldone} "${collin}\n")
#  ENDIF()
#ENDFOREACH()

# With different MAX_INDEXES values, server might behave differently in
# certain cases. 'max_indexes.inc' file should be updated accordingly to
# reflect the current MAX_INDEXES value. This file helps MTR to decide on
# which tests should be skipped.
# NOTE: While committing a patch please make sure that the file is unmodified
# and should show the default MAX_INDEXES (i.e. 64U).
IF (MAX_INDEXES)
  IF(NOT (${MAX_INDEXES} EQUAL 64U))
    FILE(WRITE include/max_indexes.inc
         "# Warning: This is an auto-generated file. Please do not modify it.
--let $max_indexes = ${MAX_INDEXES}\n")
    MESSAGE(STATUS "mysql-test/include/max_indexes.inc adjusted")
  ENDIF()
ENDIF()
