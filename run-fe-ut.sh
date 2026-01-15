#!/usr/bin/env bash
# Licensed to the Apache Software Foundation (ASF) under one
# or more contributor license agreements.  See the NOTICE file
# distributed with this work for additional information
# regarding copyright ownership.  The ASF licenses this file
# to you under the Apache License, Version 2.0 (the
# "License"); you may not use this file except in compliance
# with the License.  You may obtain a copy of the License at
#
#   http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing,
# software distributed under the License is distributed on an
# "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
# KIND, either express or implied.  See the License for the
# specific language governing permissions and limitations
# under the License.

set -eo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" &>/dev/null && pwd)"

export DORIS_HOME="${ROOT}"

. "${DORIS_HOME}/env.sh"

# Check args
usage() {
    echo "
Usage: $0 <options>
  Optional options:
     --coverage           build and run coverage statistic
     --run                build and run ut
     --skip-gen           skip generated-source.sh (use when sources already generated)
     --gradle             use Gradle for faster incremental test execution
     -j N                 run tests with N parallel threads (default: auto-detect)

  Environment variables:
     FE_UT_PARALLEL       number of parallel test threads (default: auto-detect based on CPU cores)

  Eg.
    $0                                                                      build and run ut
    $0 --coverage                                                           build and run coverage statistic
    $0 --run org.apache.doris.utframe.Demo                                  build and run the test named Demo
    $0 --run org.apache.doris.utframe.Demo#testCreateDbAndTable+test2       build and run testCreateDbAndTable in Demo test
    $0 --run org.apache.doris.Demo,org.apache.doris.Demo2                   build and run Demo and Demo2 test
    $0 --skip-gen --run org.apache.doris.Demo                               skip source generation, run Demo test
    $0 --gradle --run org.apache.doris.Demo                                 use Gradle to run Demo test (faster incremental)
    $0 -j 4 --run org.apache.doris.Demo                                     run tests with 4 parallel threads
  "
    exit 1
}

if ! OPTS="$(getopt \
    -n "$0" \
    -o 'j:' \
    -l 'coverage' \
    -l 'run' \
    -l 'skip-gen' \
    -l 'gradle' \
    -- "$@")"; then
    usage
fi

eval set -- "${OPTS}"

RUN=0
COVERAGE=0
SKIP_GEN=0
USE_GRADLE=0
PARALLEL_OVERRIDE=""

if [[ "$#" == 1 ]]; then
    #default
    RUN=0
    COVERAGE=0
else
    while true; do
        case "$1" in
        --coverage)
            COVERAGE=1
            shift
            ;;
        --run)
            RUN=1
            shift
            ;;
        --skip-gen)
            SKIP_GEN=1
            shift
            ;;
        --gradle)
            USE_GRADLE=1
            shift
            ;;
        -j)
            PARALLEL_OVERRIDE="$2"
            shift 2
            ;;
        --)
            shift
            break
            ;;
        *)
            echo "Internal error"
            exit 1
            ;;
        esac
    done
fi

echo "Build Frontend UT"

echo "******************************"
echo "    Runing DorisFe Unittest    "
echo "******************************"

#echo "Build docs"
#cd "${DORIS_HOME}/docs"
#./build_help_zip.sh
#cp build/help-resource.zip "${DORIS_HOME}"/fe/fe-core/src/test/resources/real-help-resource.zip
#cd "${DORIS_HOME}"

# Conditionally run generated-source.sh
if [[ "${SKIP_GEN}" -eq 0 ]]; then
    "${DORIS_HOME}"/generated-source.sh
else
    echo "Skipping generated-source.sh (--skip-gen specified)"
fi

cd "${DORIS_HOME}/fe"
mkdir -p build/compile

# Auto-detect parallelism based on CPU cores if not specified
if [[ -n "${PARALLEL_OVERRIDE}" ]]; then
    export FE_UT_PARALLEL="${PARALLEL_OVERRIDE}"
elif [[ -z "${FE_UT_PARALLEL}" ]]; then
    # Auto-detect: use half of CPU cores, minimum 1, maximum 8
    CPU_CORES=$(nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 2)
    AUTO_PARALLEL=$((CPU_CORES / 2))
    if [[ ${AUTO_PARALLEL} -lt 1 ]]; then
        AUTO_PARALLEL=1
    elif [[ ${AUTO_PARALLEL} -gt 8 ]]; then
        AUTO_PARALLEL=8
    fi
    export FE_UT_PARALLEL=${AUTO_PARALLEL}
fi
echo "Unit test parallel is: ${FE_UT_PARALLEL}"

# JVM optimization for faster test execution
MAVEN_OPTS="${MAVEN_OPTS:-} -XX:+UseParallelGC -XX:TieredStopAtLevel=1"
export MAVEN_OPTS

# Function to run tests with Gradle
run_gradle_test() {
    local test_class="$1"
    local gradle_wrapper="${DORIS_HOME}/fe/gradlew"

    if [[ ! -f "${gradle_wrapper}" ]]; then
        echo "Error: Gradle wrapper not found. Falling back to Maven..."
        return 1
    fi

    echo "Running tests with Gradle (incremental)..."
    local gradle_start_time=$(date +%s)

    if [[ -n "${test_class}" ]]; then
        # Convert class name to Gradle test filter format
        # org.apache.doris.Demo -> --tests "org.apache.doris.Demo"
        # org.apache.doris.Demo#testMethod -> --tests "org.apache.doris.Demo.testMethod"
        local test_filter="${test_class//#/.}"
        "${gradle_wrapper}" :fe-core:test --tests "${test_filter}" --rerun-tasks
    else
        "${gradle_wrapper}" :fe-core:test
    fi

    local gradle_end_time=$(date +%s)
    local gradle_elapsed=$((gradle_end_time - gradle_start_time))
    echo "Gradle test completed in ${gradle_elapsed} seconds"
    return 0
}

if [[ "${RUN}" -eq 1 ]]; then
    echo "Run the specified class: $1"
    # eg:
    # sh run-fe-ut.sh --run org.apache.doris.utframe.DemoTest
    # sh run-fe-ut.sh --run org.apache.doris.utframe.DemoTest#testCreateDbAndTable+test2

    if [[ "${USE_GRADLE}" -eq 1 ]]; then
        # Use Gradle for faster incremental test execution
        if ! run_gradle_test "$1"; then
            echo "Gradle test failed or not available, falling back to Maven..."
            "${MVN_CMD}" test -Dcheckstyle.skip=true -DfailIfNoTests=false -Dtest="$1"
        fi
    elif [[ "${COVERAGE}" -eq 1 ]]; then
        "${MVN_CMD}" test jacoco:report -DfailIfNoTests=false -Dtest="$1"
    else
        "${MVN_CMD}" test -Dcheckstyle.skip=true -DfailIfNoTests=false -Dtest="$1"
    fi
else
    echo "Run Frontend UT"
    if [[ "${USE_GRADLE}" -eq 1 ]]; then
        # Use Gradle for faster incremental test execution
        if ! run_gradle_test ""; then
            echo "Gradle test failed or not available, falling back to Maven..."
            "${MVN_CMD}" test -Dcheckstyle.skip=true -DfailIfNoTests=false
        fi
    elif [[ "${COVERAGE}" -eq 1 ]]; then
        "${MVN_CMD}" test jacoco:report -DfailIfNoTests=false -Dmaven.test.failure.ignore=true
    else
        "${MVN_CMD}" test -Dcheckstyle.skip=true -DfailIfNoTests=false
    fi
fi
