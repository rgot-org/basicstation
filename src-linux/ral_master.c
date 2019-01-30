// Copyright (C) 2016-2019 Semtech (International) AG. All rights reserved.
//
// This file is subject to the terms and conditions defined in file 'LICENSE',
// which is part of this source code package.

#if defined(CFG_lgw1) && defined(CFG_ral_master_slave)

#define _GNU_SOURCE
#include <limits.h>
#include <stdlib.h>
#include <stdio.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <signal.h>
#include <time.h>
#include <sys/wait.h>
#include <wordexp.h>

#include "timesync.h"
#include "tc.h"
#include "sys.h"
#include "sys_linux.h"
#include "sx1301conf.h"
#include "ral.h"
#include "ralsub.h"


#define WAIT_SLAVE_PID_INTV rt_millis(500)
#define RETRY_KILL_INTV     rt_millis(100)
#define RETRY_PIPE_IO       500
#define PPM                 1000000

typedef struct slave {
    tmr_t      tmr;
    tmr_t      tsync;
    pid_t      pid;
    aio_t*     dn;
    aio_t*     up;
    u1_t       state;
    u1_t       killCnt;
    u1_t       restartCnt;
    u1_t       antennaType;
    dbuf_t     sx1301confJson;
} slave_t;

static int    n_slaves;
static slave_t* slaves;
static pid_t    master_pid;
static u4_t     region;


// Fwd decl
static void restart_slave (tmr_t* tmr);


static int read_slave_pipe (slave_t* slave, u1_t* buf, int bufsize, int expcmd, struct ral_response* expresp) {
    u1_t slave_idx = (int)(slave-slaves);
    u1_t retries = 0;
    u1_t expok = 0;
    while(1) {
        int n = read(slave->up->fd, buf, bufsize);
        if( n == 0 ) {
            // EOF
            LOG(MOD_RAL|ERROR, "Slave (%d) - EOF", slave_idx);
            rt_yieldTo(&slave->tmr, restart_slave);
            return expok;
        }
        if( n == -1 ) {
            if( errno == EAGAIN ) {
                if( expcmd == -1 )
                    return expok; // not waiting for a synchronous answer
                if( ++retries < 5 ) {
                    rt_usleep(RETRY_PIPE_IO);
                    continue;
                }
                LOG(MOD_RAL|ERROR, "Slave (%d) did not sent reply data - expecting cmd=%d", slave_idx, expcmd);
                return expok;
            }
            rt_fatal("Slave (%d) pipe read fail: %s", slave_idx, strerror(errno));
            // NOT REACHED
        }
        slave->restartCnt = 0;
        int off = 0;
        while( off < n ) {
            struct ral_header* hdr = (struct ral_header*)&buf[off];
            if( expcmd >= 0 && n >= off+sizeof(struct ral_response) && hdr->cmd == expcmd ) {
                *expresp = *(struct ral_response*)hdr;
                off += sizeof(*expresp);
                expok = 1;
                expcmd = -1;
            }
            else if( n >= off+sizeof(struct ral_timesync_resp) && hdr->cmd == RAL_CMD_TIMESYNC ) {
                struct ral_timesync_resp* resp = (struct ral_timesync_resp*)hdr;
                ustime_t delay = ts_updateTimesync(slave_idx, resp->quality, &resp->timesync);
                rt_setTimer(&slave->tsync, rt_micros_ahead(delay));
                off += sizeof(*resp);
            }
            else if( n >= off+sizeof(struct ral_rx_resp) && hdr->cmd == RAL_CMD_RX ) {
                struct ral_rx_resp* resp = (struct ral_rx_resp*)hdr;
                rxjob_t* rxjob = !TC ? NULL : s2e_nextRxjob(&TC->s2ctx);
                if( rxjob != NULL ) {
                    memcpy(&TC->s2ctx.rxq.rxdata[rxjob->off], resp->rxdata, resp->rxlen);
                    rxjob->len = resp->rxlen;
                    rxjob->freq = resp->freq;
                    rxjob->rctx = resp->rctx;
                    rxjob->xtime = resp->xtime;
                    rxjob->rssi = resp->rssi;
                    rxjob->snr = resp->snr;
                    rxjob->dr = s2e_rps2dr(&TC->s2ctx, resp->rps);
                    if( rxjob->dr == DR_ILLEGAL ) {
                        LOG(MOD_RAL|ERROR, "Unable to map to an up DR: %R", resp->rps);
                    } else {
                        s2e_addRxjob(&TC->s2ctx, rxjob);
                        s2e_flushRxjobs(&TC->s2ctx); // XXX
                    }
                } else {
                    LOG(MOD_RAL|ERROR, "Slave (%d) has RX frame dropped - out of space", slave_idx);
                }
                off += sizeof(*resp);
            }
            else {
                
                rt_fatal("Slave (%d) sent unexpected data: cmd=%d size=%d", slave_idx, hdr->cmd, n-off);
            }
        }
        assert(off==n);
    }
}


static void pipe_read (aio_t* aio) {
    slave_t* slave = aio->ctx;
    u1_t buf[PIPE_BUF];
    struct ral_response resp;
    read_slave_pipe(slave, buf, PIPE_BUF, -1, &resp);
}


// Called at exit for master process - kill all children
static void killAllSlaves () {
    if( master_pid != getpid() )
        return;  // do not run in fork's of master intending to run commands!

    for( int i=0; i<n_slaves; i++ ) {
        slave_t* slave = &slaves[i];
        pid_t pid = slave->pid;
        slave->pid = 0;
        aio_close(slave->up);
        aio_close(slave->dn);
        rt_clrTimer(&slave->tmr);
        if( pid )
            kill(pid, SIGKILL);
    }
    rt_usleep(rt_millis(200));
    while( waitpid(-1, NULL, WNOHANG) > 0 );
}


static int is_slave_alive (slave_t* slave) {
    int wstatus, code = 0;
    str_t msg, xmsg = "";
    int slave_idx = slave-slaves;
    pid_t pid = slave->pid;
    if( pid == 0 )
        return 0;
    pid_t wpid = waitpid(pid, &wstatus, WNOHANG);
    if( wpid < 0 ) {
        msg = "Assuming slave is dead - waitpid errno";
        xmsg = strerror(errno);
        goto gone;
    }
    if( wpid == pid ) {
        if( WIFEXITED(wstatus) ) {
            code = WEXITSTATUS(wstatus);
            if( code >= FATAL_GENERIC && code <= FATAL_MAX ) {
                rt_fatal("Slave pid=%d idx=%d: Fatal exit", pid, slave_idx);
            }
            msg = "Exited with status";
            goto gone;
        }
        if( WIFSIGNALED(wstatus) ) {
            code = WTERMSIG(wstatus);
            msg = "Terminated by signal";
            goto gone;
        }
        // not handling: WIFSTOPPED/WIFCONTINUED
        return 1; // still running
    }
    if( wpid != 0 )
        LOG(MOD_RAL|WARNING, "waitpid returned unexpected pid=%d", wpid);
    return 1; // still running

 gone:
    LOG(MOD_RAL|ERROR, "Slave pid=%d idx=%d: %s=%d %s", pid, slave_idx, msg, code, xmsg);
    slave->pid = pid = 0;
    return 0;
}


static void recheck_slave (tmr_t* tmr) {
    slave_t* slave = memberof(slave_t, tmr, tmr);
    if( is_slave_alive(slave) ) {
        rt_setTimer(&slave->tmr, rt_micros_ahead(WAIT_SLAVE_PID_INTV));
        return;
    }
    restart_slave(tmr);
}


static void execSlave (int idx, int rdfd, int wrfd) {
    wordexp_t wexp;
    memset(&wexp, 0, sizeof(wexp));

    // Prepare some env vars
    char idxbuf[12], rdfdbuf[12], wrfdbuf[12];
    snprintf(idxbuf,  sizeof(idxbuf),  "%d", idx);
    snprintf(rdfdbuf, sizeof(rdfdbuf), "%d", rdfd);
    snprintf(wrfdbuf, sizeof(wrfdbuf), "%d", wrfd);
    setenv("SLAVE_IDX" , idxbuf , 1);
    setenv("SLAVE_RDFD", rdfdbuf, 1);
    setenv("SLAVE_WRFD", wrfdbuf, 1);
    int fail = wordexp(sys_slaveExec, &wexp, WRDE_DOOFFS|WRDE_NOCMD|WRDE_UNDEF|WRDE_SHOWERR);
    if( fail ) {
        str_t err;
        switch(fail) {
        case WRDE_BADCHAR: err = "Unquoted shell special character (e.g. one of <>|&;(){} -- use quotes?)"; break;
        case WRDE_BADVAL:  err = "Undefined shell variable"; break;
        case WRDE_CMDSUB:  err = "Command substition $(..) not allowed"; break;
        case WRDE_SYNTAX:  err = "Syntax error"; break;
        default: err = "Unknown error"; break;
        }
        wordfree(&wexp);
        rt_fatal("Failed to execute slave process: %s", err);
    }
    for( int i=0; i<=wexp.we_wordc; i++ ) {
        LOG(MOD_RAL|DEBUG, "%s argv[%d]: <%s>\n", i==0?"execvp":"      ", i, wexp.we_wordv[i]);
    }
    sys_flushLog();
    execvp(wexp.we_wordv[0], wexp.we_wordv);
    for( int i=0; i<=wexp.we_wordc; i++ ) {
        LOG(MOD_RAL|ERROR, "%s argv[%d]: <%s>\n", i==0?"execvp":"      ", i, wexp.we_wordv[i]);
    }
    rt_fatal("Failed to execute slave process (%d): %s", idx, strerror(errno));
}


static int write_slave_pipe (slave_t* slave, void* data, int len) {
    if( slave->dn == NULL ) {
        LOG(MOD_RAL|ERROR, "Slave currently down/restarting");
        return 0;
    }
    int n, retries = 0;
 again:
    n = write(slave->dn->fd, data, len);
    if( n != -1 ) {
        assert(n==len);
        return 1;
    }
    if( errno == EAGAIN ) {
        if( ++retries < 5 ) {
            rt_usleep(RETRY_PIPE_IO);
            goto again;
        }
        LOG(MOD_RAL|ERROR, "Pipe to slave full");
    }
    else if( errno == EPIPE ) {
        LOG(MOD_RAL|ERROR, "Slave pipe dead");
    }
    else {
        LOG(MOD_RAL|ERROR, "Slave pipe write error: %s", strerror(errno));
    }
    return 0;
}


static void send_config (slave_t* slave) {
    struct ral_config_req req = { .cmd = RAL_CMD_CONFIG, .rctx = 0 };
    strcpy(req.hwspec, "sx1301/1");
    int jlen = slave->sx1301confJson.bufsize;
    if( jlen > sizeof(req.json) )
        rt_fatal("JSON of sx1301conf to big for pipe: %d > %d", jlen, sizeof(req.json));
    if( jlen > 0 ) {
        req.region = region;
        req.jsonlen = jlen;
        memcpy(req.json, slave->sx1301confJson.buf, jlen);
        LOG(MOD_RAL|INFO, "Master sending %d bytes of JSON sx1301conf to slave (%d)", jlen, (int)(slave-slaves));
        if( !write_slave_pipe(slave, &req, sizeof(req)) )
            rt_fatal("Failed to send sx1301conf");
    }
}


static void req_timesync (tmr_t* tmr) {
    slave_t* slave = memberof(slave_t, tmr, tsync);
    struct ral_timesync_req req = { .cmd = RAL_CMD_TIMESYNC, .rctx = 0 };
    if( !write_slave_pipe(slave, &req, sizeof(req)) )
        rt_fatal("Failed to send ral_timesync_req");
}


static void restart_slave (tmr_t* tmr) {
    slave_t* slave = memberof(slave_t, tmr, tmr);
    pid_t pid = slave->pid;
    int slaveIdx = (int)(slave-slaves);

    if( ++slave->restartCnt > 4 ) {
        // Velocity check for slave restarts
        rt_fatal("Slave %d restarted %d times without successful interaction",
                 slaveIdx, slave->restartCnt);
    }
    rt_clrTimer(&slave->tmr);
    rt_clrTimer(&slave->tsync);
    aio_close(slave->up);
    aio_close(slave->dn);
    slave->up = slave->dn = NULL;

    if( is_slave_alive(slave) ) {
        LOG(MOD_RAL|INFO, "Slave pid=%d idx=%d: Trying kill (cnt=%d)", slaveIdx, pid, slave->killCnt);
        int err = kill(pid, slave->killCnt <= 2 ? SIGTERM : SIGKILL);
        if( err == -1 && errno != ESRCH )
            LOG(MOD_RAL|ERROR, "kill failed: %s", strerror(errno));
        slave->killCnt += 1;
        rt_setTimerCb(&slave->tmr, rt_micros_ahead(RETRY_KILL_INTV), restart_slave);
        return;
    }
    int up[2] = { -1, -1 };
    int dn[2] = { -1, -1 };
    if( pipe2(up, O_NONBLOCK) == -1 || pipe2(dn, O_NONBLOCK) == -1 ) {
        rt_fatal("Failed to create pipe: %s", strerror(errno));
    }
    slave->up = aio_open(slave, up[0], pipe_read, NULL);
    slave->dn = aio_open(slave, dn[1], NULL, NULL);  // we need this only for O_CLOEXEC
    sys_flushLog();

    if( (pid = fork()) == 0 ) {
        // This is the child process.  Execute the shell command.
        execSlave(slaveIdx, dn[0], up[1]);
        // NOT REACHED
        assert(0);
    }
    else if( pid < 0 ) {
        rt_fatal("Fork failed: %s\n", strerror(errno));
    }
    // Master
    LOG(MOD_RAL|INFO, "Master has started slave: pid=%d idx=%d (attempt %d)", pid, slaveIdx, slave->restartCnt);
    close(up[1]);
    close(dn[0]);
    slave->pid = pid;
    send_config(slave);
    pipe_read(slave->up);
    rt_yieldTo(&slave->tmr, recheck_slave);
}


u1_t ral_altAntennas (u1_t txunit) {
    if( txunit >= n_slaves || slaves[txunit].antennaType != SX1301_ANT_OMNI )
        return 0;
    u1_t v = 0;
    for( int sidx=0; sidx < n_slaves; sidx++ ) {
        if( sidx == txunit || slaves[sidx].antennaType != SX1301_ANT_OMNI )
            continue;
        v |= 1<<sidx;
    }
    return v;
}


static slave_t* txunit2slave(u1_t txunit, str_t op) {
    if( txunit >= n_slaves ) {
        LOG(MOD_RAL|ERROR, "Illegal radio txunit #%d - rejecting %s", txunit, op);
        return NULL;
    }
    slave_t* slave = &slaves[txunit];
    if( slave->dn == NULL ) {
        LOG(MOD_RAL|ERROR, "Slave #%d dead - rejecting %s", txunit, op);
        return NULL;
    }
    return slave;
}


int ral_tx (txjob_t* txjob, s2ctx_t* s2ctx, int nocca) {
    // NOTE: nocca not possible to implement with current libloragw API
    slave_t* slave = txunit2slave(txjob->txunit, "tx");
    if( slave == NULL )
        return RAL_TX_FAIL;
    struct ral_tx_req req;
    memset(&req, 0, sizeof(req));
    req.cmd = nocca ? RAL_CMD_TX_NOCCA : RAL_CMD_TX;
    req.rctx = txjob->rctx;
    req.rps = (s2e_dr2rps(s2ctx, txjob->dr)
               | (txjob->txflags & TXFLAG_BCN ? RPS_BCN : 0));
    req.xtime = txjob->xtime;
    req.freq = txjob->freq;
    req.txpow = txjob->txpow;
    req.txlen = txjob->len;
    memcpy(req.txdata, &s2ctx->txq.txdata[txjob->off], txjob->len);
    if( !write_slave_pipe(slave, &req, sizeof(req)) )
        return RAL_TX_FAIL;
    if( region == 0 )
        return RAL_TX_OK;
    struct ral_response resp;
    u1_t buf[PIPE_BUF];
    if( !read_slave_pipe(slave, buf, PIPE_BUF, RAL_CMD_TX, &resp) )
        return TXSTATUS_IDLE;
    return resp.status;
}


int ral_txstatus (u1_t txunit) {
    slave_t* slave = txunit2slave(txunit, "tx");
    if( slave == NULL )
        return TXSTATUS_IDLE;
    struct ral_txstatus_req req = { .cmd = RAL_CMD_TXSTATUS, .rctx = txunit };
    if( !write_slave_pipe(slave, &req, sizeof(req)) )
        return TXSTATUS_IDLE;
    struct ral_response resp;
    u1_t buf[PIPE_BUF];
    if( !read_slave_pipe(slave, buf, PIPE_BUF, RAL_CMD_TXSTATUS, &resp) )
        return TXSTATUS_IDLE;
    return resp.status;
}


void ral_txabort (u1_t txunit) {
    slave_t* slave = txunit2slave(txunit, "tx");
    if( slave == NULL )
        return;
    struct ral_txstatus_req req = { .cmd = RAL_CMD_TXABORT, .rctx = txunit };
    write_slave_pipe(slave, &req, sizeof(req));
}


int ral_config (str_t hwspec, u4_t cca_region, char* json, int jsonlen) {
    if( strncmp(hwspec, "sx1301/", 7) != 0 ) {
        LOG(MOD_RAL|ERROR, "Unsupported hwspec=%s", hwspec);
        return 0;
    }
    for( int i=0; i<n_slaves; i++ )
        dbuf_free(&slaves[i].sx1301confJson);

    ujdec_t D;
    uj_iniDecoder(&D, json, jsonlen);
    if( uj_decode(&D) ) {
        LOG(MOD_RAL|ERROR, "Parsing of sx1301 channel setup JSON failed");
        return 0;
    }
    if( uj_null(&D) ) {
        LOG(MOD_RAL|ERROR, "sx1301_conf is null but a hw setup IS required - no fallbacks");
        return 0;
    }
    uj_enterArray(&D);
    int slaveIdx, n1301=0;
    while( (slaveIdx = uj_nextSlot(&D)) >= 0 ) {
        n1301 = slaveIdx+1;
        if( slaveIdx < n_slaves ) {
            slaves[slaveIdx].sx1301confJson = dbuf_dup(uj_skipValue(&D));
        } else {
            uj_skipValue(&D);
        }
    }
    uj_exitArray(&D);
    uj_assertEOF(&D);
    if( n1301 == 0 ) {
        LOG(MOD_RAL|ERROR, "sx1301_conf is empty but a hw setup IS required - no fallbacks");
        return 0;
    }
    str_t s = hwspec+7;
    int specn = rt_readDec(&s);
    if( specn != n1301 ) {
        LOG(MOD_RAL|ERROR, "hwspec=%s and size of sx1301_conf array (%d) not in sync", hwspec, n1301);
        return 0;
    }
    if( n1301 > n_slaves ) {
        LOG(MOD_RAL|ERROR, "Region plan asks for hwspec=%s which exceeds actual hardware: sx1301/%d", hwspec, n1301);
        return 0;
    }
    if( n1301 < n_slaves ) {
        if( n_slaves % n1301 != 0 ) {
            LOG(MOD_RAL|WARNING, "Region plan hwspec '%s' cannot be replicated onto routers 'sx1301/%d' - router is underutilized",
                hwspec, n_slaves);
        } else {
            for( int si=n1301, sj=0; si < n_slaves; si++, sj=(sj+1)%n1301 )
                slaves[si].sx1301confJson = dbuf_dup(slaves[sj].sx1301confJson);
            LOG(MOD_RAL|WARNING, "Region plan hwspec '%s' replicated %d times onto slaves 'sx1301/%d' - assuming antenna diversity",
                hwspec, n_slaves/n1301, n_slaves);
        }
    } else {
        LOG(MOD_RAL|INFO, "Region plan hwspec '%s' mapped to %d slaves 'sx1301/1'", hwspec, n_slaves);
    }
    region = cca_region;
    for( int i=0; i < n_slaves; i++ )
        send_config(&slaves[i]);
    return 1;
}


void ral_ini () {
    // Find out if we should start slaves
    int slaveCnt = 0;
    while(1) {
        char cfname[64];
        snprintf(cfname, sizeof(cfname), "slave-%d.conf", slaveCnt);
        dbuf_t b = sys_checkFile(cfname);
        if( b.buf == NULL )
            break;
        free(b.buf);
        slaveCnt += 1;
    }
    if( slaveCnt == 0 || slaveCnt > MAX_TXUNITS )
        rt_fatal("%s 'slave-N.conf' files found  (N=0,1,..,%d)",
                 slaveCnt ? "Too many" : "No", MAX_TXUNITS-1);

    assert(slaves == NULL);
    n_slaves = slaveCnt;
    slaves = rt_mallocN(slave_t, n_slaves);
    int allok = 1;
    for( int sidx=0; sidx < n_slaves; sidx++ ) {
        struct sx1301conf sx1301conf;
        if( !sx1301conf_parse_setup(&sx1301conf, sidx, "sx1301/1", "{}", 2) ) {
            allok = 0;
        } else {
            slaves[sidx].antennaType = sx1301conf.antennaType;
        }
    }
    if( !allok )
        rt_fatal("Failed to load/parse some slave config files");

    master_pid = getpid();
    atexit(killAllSlaves);
    signal(SIGPIPE, SIG_IGN);

    for( int i=0; i<n_slaves; i++ ) {
        rt_iniTimer(&slaves[i].tmr, NULL);
        rt_iniTimer(&slaves[i].tsync, req_timesync);
        rt_yieldTo(&slaves[i].tmr, restart_slave);
    }
}


void ral_stop () {
    struct ral_timesync_req req = { .cmd = RAL_CMD_STOP, .rctx = 0 };
    for( int slaveIdx=0; slaveIdx < n_slaves; slaveIdx++ ) {
        slave_t* slave = &slaves[slaveIdx];
        write_slave_pipe(slave, &req, sizeof(req));
    }
}


#endif // defined(CFG_lgw1) && defined(CFG_ral_master_slave)