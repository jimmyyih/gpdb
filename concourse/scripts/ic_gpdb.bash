#!/bin/bash -l

set -eox pipefail

TOP_DIR=$(pwd)
TOP_DIR_BASENAME=$( basename `pwd` )
CWDIR="$( cd "$( dirname "${BASH_SOURCE[0]}" )" && pwd )"
source "${CWDIR}/common.bash"

function gen_env(){
  cat > /opt/run_test.sh <<-EOF
		trap look4diffs ERR

		function look4diffs() {

		    diff_files=\`find .. -name regression.diffs\`

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
		cd "\${1}/gpdb_src"
		source gpAux/gpdemo/gpdemo-env.sh
		make ${MAKE_TEST_COMMAND}
	EOF

	chmod a+x /opt/run_test.sh
}

function setup_gpadmin_user() {
    ./gpdb_src/concourse/scripts/setup_gpadmin_user.bash "$TEST_OS"
}

function _main() {
    if [ -z "${MAKE_TEST_COMMAND}" ]; then
        echo "FATAL: MAKE_TEST_COMMAND is not set"
        exit 1
    fi

    if [ -z "$TEST_OS" ]; then
        echo "FATAL: TEST_OS is not set"
        exit 1
    fi

    if [ "$TEST_OS" != "centos" -a "$TEST_OS" != "sles" ]; then
        echo "FATAL: TEST_OS is set to an invalid value: $TEST_OS"
	echo "Configure TEST_OS to be centos or sles"
        exit 1
    fi

    if [ "$( command -v lcov )" == "" ]; then
        yum install -y lcov
    fi

    # This ugly block exists since sles11 installs kerberos at a different path that is a test-only dependency
    if [ "$TEST_OS" == "sles" ]; then
      zypper addrepo --no-gpgcheck http://download.opensuse.org/distribution/11.4/repo/oss/ ossrepo
      zypper -n install krb5 krb5-client krb5-server
      cp /usr/lib/mit/sbin/krb5kdc /usr/sbin/
      export PATH=/usr/lib/mit/sbin/:$PATH
      # Kerberos upgrades perl, so we have to re-do the symlink. Ideally, we
      # wouldn't have a symlink at all, but this step is necessary as long as
      # we build against a different version of perl, and explicitly link to
      # that version.
      # See gpdb/src/pl/plperl/GNUmakefile line 22:
      # perl_lib := $(basename $(notdir $(wildcard $(perl_archlibexp)/CORE/perl[5-9]*.lib)))
      local libperl_path
      libperl_path="$(rpm -ql perl | grep libperl.so)"
      ln -sf "$libperl_path" /lib64/libperl.so || return 1
    fi

    time setup_gpadmin_user
    time configure
    time install_gpdb_src_compiled
    time install_gpdb
    time make_cluster
    time gen_env
    time run_test

    if [ "${TEST_BINARY_SWAP}" == "true" ]; then
        time ./gpdb_src/concourse/scripts/test_binary_swap_gpdb.bash
    fi

    GCOV_COMPILED_DIR=$( ls /tmp/build | grep -v $TOP_DIR_BASENAME )
    pushd /tmp/build/$GCOV_COMPILED_DIR
    geninfo --rc lcov_branch_coverage=1 --no-external -o ./cov_ic.info .
    lcov --rc lcov_branch_coverage=1 --add-tracefile cov_ic.info --no-external -o cov_merged.info
    if [ ! -d $TOP_DIR/gpdb_artifacts ]; then
        mkdir $TOP_DIR/gpdb_artifacts
    fi
    cp cov_ic.info $TOP_DIR/gpdb_artifacts/.
    popd
}

_main "$@"
