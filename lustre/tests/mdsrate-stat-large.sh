#!/bin/sh
#
# This test was used in a set of CMD3 tests (cmd3-8 test).

# File attribute retrieval rate for large file creation
# 3300 ops/sec/OST for single node 28500 ops/sec/OST aggregate

# In a dir containing 10 million striped files, the mdsrate Test Program will
# perform directory ordered stat's (readdir) for 10 minutes. This test will be
# run from a single node for #1 and from all nodes for #2 aggregate test to
# measure stat performance.  

LUSTRE=${LUSTRE:-`dirname $0`/..}
. $LUSTRE/tests/test-framework.sh
init_test_env $@
. ${CONFIG:=$LUSTRE/tests/cfg/$NAME.sh}

assert_env CLIENTS MDSRATE SINGLECLIENT MPIRUN

MACHINEFILE=${MACHINEFILE:-$TMP/$(basename $0 .sh).machines}
# Do not use name [df][0-9]* to avoid cleanup by rm, bug 18045
TESTDIR=$MOUNT/mdsrate

# Requirements
NUM_FILES=${NUM_FILES:-1000000}
TIME_PERIOD=${TIME_PERIOD:-600}                        # seconds

# --random_order (default) -OR- --readdir_order
DIR_ORDER=${DIR_ORDER:-"--readdir_order"}

LOG=${TESTSUITELOG:-$TMP/$(basename $0 .sh).log}
CLIENT=$SINGLECLIENT
NODES_TO_USE=${NODES_TO_USE:-$CLIENTS}
NUM_CLIENTS=$(get_node_count ${NODES_TO_USE//,/ })

rm -f $LOG

[ ! -x ${MDSRATE} ] && error "${MDSRATE} not built."

log "===== $0 ====== " 

check_and_setup_lustre
mkdir -p $TESTDIR
chmod 0777 $TESTDIR

IFree=$(inodes_available)
if [ $IFree -lt $NUM_FILES ]; then
    NUM_FILES=$IFree
fi

generate_machine_file $NODES_TO_USE $MACHINEFILE || error "can not generate machinefile"

$LFS setstripe $TESTDIR -c -1
get_stripe $TESTDIR

if [ -n "$NOCREATE" ]; then
    echo "NOCREATE=$NOCREATE  => no file creation."
else
    mdsrate_cleanup $NUM_CLIENTS $MACHINEFILE $NUM_FILES $TESTDIR 'f%%d' --ignore

    log "===== $0 Test preparation: creating ${NUM_FILES} files."
    echo "Test preparation: creating ${NUM_FILES} files."

    COMMAND="${MDSRATE} ${MDSRATE_DEBUG} --create --dir ${TESTDIR}
                        --nfiles ${NUM_FILES} --filefmt 'f%%d'"
    echo "+" ${COMMAND}

    NUM_CLIENTS=$(get_node_count ${NODES_TO_USE//,/ })
    NUM_THREADS=$((NUM_CLIENTS * MDSCOUNT))
    if [ $NUM_CLIENTS -gt 50 ]; then
        NUM_THREADS=$NUM_CLIENTS
    fi

    mpi_run -np ${NUM_THREADS} -machinefile ${MACHINEFILE} ${COMMAND} 2>&1
    [ ${PIPESTATUS[0]} != 0 ] && error "mpirun ... mdsrate ... file creation failed, aborting"

fi

COMMAND="${MDSRATE} ${MDSRATE_DEBUG} --stat --time ${TIME_PERIOD}
        --dir ${TESTDIR} --nfiles ${NUM_FILES} --filefmt 'f%%d'
        ${DIR_ORDER} ${SEED_OPTION}"

# 1
if [ -n "$NOSINGLE" ]; then
    echo "NO Test for stats on a single client."
else
    log "===== $0 ### 1 NODE STAT ###"
    echo "Running stats on 1 node(s)."
    echo "+" ${COMMAND}

    mpi_run -np 1 -machinefile ${MACHINEFILE} ${COMMAND} | tee ${LOG}

    if [ ${PIPESTATUS[0]} != 0 ]; then
        [ -f $LOG ] && cat $LOG
        error "mpirun ... mdsrate ... failed, aborting"
    fi
fi

# 2
if [ -n "$NOMULTI" ]; then
    echo "NO test for stats on multiple nodes."
else
    log "===== $0 ### ${NUM_CLIENTS} NODES STAT ###"
    echo "Running stats on ${NUM_CLIENTS} node(s)."
    echo "+" ${COMMAND}

    NUM_THREADS=$(get_node_count ${NODES_TO_USE//,/ })
    mpi_run -np ${NUM_THREADS} -machinefile ${MACHINEFILE} ${COMMAND} | tee ${LOG}

    if [ ${PIPESTATUS[0]} != 0 ]; then
        [ -f $LOG ] && cat $LOG
        error "mpirun ... mdsrate ... failed, aborting"
    fi
fi

equals_msg `basename $0`: test complete, cleaning up
mdsrate_cleanup $NUM_CLIENTS $MACHINEFILE $NUM_FILES $TESTDIR 'f%%d'
rm -f $MACHINEFILE
check_and_cleanup_lustre
#rm -f $LOG

exit 0
