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

# fib(15) = 610.  The program creates no data block, so no coherence cell can
# change its outcome.
arts_example(NAME fib
    SOURCES fib.c
    ARGS_SMOKE 15
    PASS_REGEX "Fib 15: 610 time: "
    BLOCK_FREE)

# fib(15) = 610 again, each value carried in a data block.
arts_example(NAME fib_db
    SOURCES fib_db.c
    ARGS_SMOKE 15
    PASS_REGEX "Fib 15: 610 time: ")

# The sum of the rank ids, n(n-1)/2 on n ranks: 0 on one, 1 on two, 6 on four.
arts_example(NAME reduction
    SOURCES reduction.c
    PASS_REGEX_SINGLE "Result 0[^0-9]"
    PASS_REGEX "Result 1[^0-9]")
if(TEST reduction_example_4n_4w_1p)
    set_tests_properties(reduction_example_4n_4w_1p PROPERTIES
        PASS_REGULAR_EXPRESSION "Result 6[^0-9]")
endif()

# 8-queens has 92 solutions; the program checks its count against its table.
arts_example(NAME nqueens
    SOURCES nqueens.c
    ARGS_SMOKE 8
    PASS_REGEX "Result verified!")
