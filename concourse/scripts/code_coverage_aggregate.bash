#!/bin/bash -l

set -eox pipefail

function setup_gpadmin_user() {
    ./gpdb_src/concourse/scripts/setup_gpadmin_user.bash "$TEST_OS"
}

function _main() {
    if [ "$( command -v lcov )" == "" ]; then
        yum install -y lcov
    fi

    local add_tracefile_string = ""
    for i in `ls *.info`; do
        add_tracefile_string="${add_tracefile_string} --add-tracefile ${i}"
    done
    lcov --rc lcov_branch_coverage=1 ${add_tracefile_string} --no-external -o coverage_aggregate.info
    genhtml --legend --branch-coverage --output-directory lcov-html coverage_aggregate.info

    ## upload generated html to website hosting enabled S3 bucket and print URL
}

_main "$@"
