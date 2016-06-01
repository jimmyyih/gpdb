#!/bin/bash -l

set -eox pipefail

CWDIR="$( cd "$( dirname "${BASH_SOURCE[0]}" )" && pwd )"
source "${CWDIR}/common.bash"

function gen_env(){
  cat > /opt/run_test.sh <<-EOF
		trap look4diffs ERR

		function look4diffs() {

		    diff_files="../src/test/regress/regression.diffs
				../src/test/regress/bugbuster/regression.diffs"

		    for diff_file in \${diff_files}; do
			if [ -f "\${diff_file}" ]; then
			    cat <<-FEOF

						======================================================================
						DIFF FILE: \${diff_file}
						----------------------------------------------------------------------

						\$(cat "\${diff_file}")

					FEOF
			fi
		    done
		    exit 1
		}
		source /usr/local/greenplum-db-devel/greenplum_path.sh
		source /opt/gcc_env.sh
		cd "\${1}/gpdb4_src/gpAux"
		source gpdemo/gpdemo-env.sh
		make ${MAKE_TEST_COMMAND}
	EOF

	chmod a+x /opt/run_test.sh
}

function _main() {

    if [ -z "${MAKE_TEST_COMMAND}" ]; then
        echo "FATAL: MAKE_TEST_COMMAND is not set"
        exit 1
    fi

    time install_sync_tools
    time configure
    time install_gpdb
    time ./gpdb4_src/ci/concourse/scripts/setup_gpadmin_user.bash
    time make_cluster
    time gen_env
    time run_test
}

_main "$@"
