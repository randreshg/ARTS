###############################################################################
## This material was prepared as an account of work sponsored by an agency
## of the United States Government.  Neither the United States Government
## nor the United States Department of Energy, nor Battelle, nor any of
## their employees, nor any jurisdiction or organization that has cooperated
## in the development of these materials, makes any warranty, express or
## implied, or assumes any legal liability or responsibility for the accuracy,
## completeness, or usefulness or any information, apparatus, product,
## software, or process disclosed, or represents that its use would not
## infringe privately owned rights.
##
## Reference herein to any specific commercial product, process, or service
## by trade name, trademark, manufacturer, or otherwise does not necessarily
## constitute or imply its endorsement, recommendation, or favoring by the
## United States Government or any agency thereof, or Battelle Memorial
## Institute. The views and opinions of authors expressed herein do not
## necessarily state or reflect those of the United States Government or
## any agency thereof.
##
##                      PACIFIC NORTHWEST NATIONAL LABORATORY
##                                  operated by
##                                    BATTELLE
##                                    for the
##                      UNITED STATES DEPARTMENT OF ENERGY
##                         under Contract DE-AC05-76RL01830
##
## Copyright 2019 Battelle Memorial Institute
## Licensed under the Apache License, Version 2.0 (the "License");
## you may not use this file except in compliance with the License.
## You may obtain a copy of the License at
##
##    https://www.apache.org/licenses/LICENSE-2.0
##
## Unless required by applicable law or agreed to in writing, software
## distributed under the License is distributed on an "AS IS" BASIS, WITHOUT
## WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied. See the
## License for the specific language governing permissions and limitations
###############################################################################

# The checked-in smoke graph: 16 vertices, 23 undirected edges, each listed
# once, so every program reads it with --flip.  Its answers come from
# examples/inputs/graph_reference.py and depend on no rank count.
get_filename_component(_graph "${CMAKE_CURRENT_SOURCE_DIR}/../inputs/graph_smoke.txt" ABSOLUTE)
set(_graph_args --file ${_graph} --num-vertices 16 --num-edges 23 --flip)

# bfs prints the levels of rank 0's vertices only, so its answer is pinned
# where rank 0 holds every vertex: the single-node test, which must print all
# sixteen levels from source 0 in order (vertex 15 is unreachable).
set(_bfs_levels 0 1 1 1 2 1 2 3 4 2 1 6 4 5 6 18446744073709551615)
set(_bfs_pin "Printing vertex levels")
set(_v 0)
foreach(_level IN LISTS _bfs_levels)
    string(APPEND _bfs_pin ".*Local vertex : ${_v}, Level : ${_level}[^0-9]")
    math(EXPR _v "${_v} + 1")
endforeach()
arts_example(NAME bfs
    SOURCES bfs.c
    ARGS_SMOKE ${_graph_args} --source 0
    PASS_REGEX "Printing vertex levels"
    PASS_REGEX_SINGLE "${_bfs_pin}")

# Ten triangles, four of them with the last vertex of a partition at four ranks
# (3, 7 or 11) as their smallest vertex.
arts_example(NAME triangle_count
    SOURCES triangle_count.c
    ARGS_SMOKE ${_graph_args}
    PASS_REGEX "Triangle Count: 10 Time: ")

# Vertex 3: degree 6 and six triangles, 6 + 2 * 6 = 18.
arts_example(NAME scan_statistics
    SOURCES scan_statistics.cpp
    ARGS_SMOKE ${_graph_args}
    PASS_REGEX "Max vertex_t: 3 scan_stat: 18[^0-9]")
