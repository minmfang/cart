#!/usr/bin/env python3
# Copyright (C) 2017 Intel Corporation
# All rights reserved.
#
# Redistribution and use in source and binary forms, with or without
# modification, are permitted for any purpose (including commercial purposes)
# provided that the following conditions are met:
#
# 1. Redistributions of source code must retain the above copyright notice,
#    this list of conditions, and the following disclaimer.
#
# 2. Redistributions in binary form must reproduce the above copyright notice,
#    this list of conditions, and the following disclaimer in the
#    documentation and/or materials provided with the distribution.
#
# 3. In addition, redistributions of modified forms of the source or binary
#    code must carry prominent notices stating that the original code was
#    changed and the date of the change.
#
#  4. All publications or advertising materials mentioning features or use of
#     this software are asked, but not required, to acknowledge that it was
#     developed by Intel Corporation and credit the contributors.
#
# 5. Neither the name of Intel Corporation, nor the name of any Contributor
#    may be used to endorse or promote products derived from this software
#    without specific prior written permission.
#
# THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
# AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
# IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
# ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER BE LIABLE FOR ANY
# DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
# (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
# LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
# ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
# (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
# THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
# -*- coding: utf-8 -*-
"""
Threaded client/server sharing context test

Usage:

Execute from the install/$arch/TESTING directory. The results are placed in the
testLogs/testRun/threaded_test directory. Any threaded_test output is under
<file yaml>_loop#/<module.name.execStrategy.id>/1(process set)/rank<number>.
There you will find anything written to stdout and stderr. The output from
memcheck and callgrind are in the threaded_test directory. At the end of a test
run, the last testRun directory is renamed to testRun_<date stamp>

python3 test_runner scripts/cart_threaded_test.yml

To use valgrind memory checking
set TR_USE_VALGRIND in cart_threaded_test.yml to memcheck

To use valgrind call (callgrind) profiling
set TR_USE_VALGRIND in cart_threaded_test.yml to callgrind

"""

import os
import time
import commontestsuite

class TestThreaded(commontestsuite.CommonTestSuite):
    """ Execute process set tests """
    def setUp(self):
        """setup the test"""
        self.get_test_info()
        log_mask = os.getenv("D_LOG_MASK", "INFO")
        crt_phy_addr = os.getenv("CRT_PHY_ADDR_STR", "ofi+sockets")
        ofi_interface = os.getenv("OFI_INTERFACE", "eth0")
        ofi_share_addr = os.getenv("CRT_CTX_SHARE_ADDR", "0")
        ofi_ctx_num = os.getenv("CRT_CTX_NUM", "0")
        baseport = self.generate_port_numbers(ofi_interface)
        self.pass_env = ' -x D_LOG_MASK={!s} -x CRT_PHY_ADDR_STR={!s}' \
                        ' -x OFI_INTERFACE={!s} -x OFI_PORT={!s}' \
                        ' -x CRT_CTX_SHARE_ADDR={!s} -x CRT_CTX_NUM={!s}' \
                        .format(log_mask, crt_phy_addr, ofi_interface, \
                                baseport, ofi_share_addr, ofi_ctx_num)

    def tearDown(self):
        """tear down the test"""
        self.logger.info("tearDown begin")
        os.environ.pop("CRT_PHY_ADDR_STR", "")
        os.environ.pop("OFI_INTERFACE", "")
        os.environ.pop("D_LOG_MASK", "")
        self.free_port()
        self.logger.info("tearDown end\n")

    def test_threaded_one_node(self):
        """Simple threaded test one node"""
        testmsg = self.shortDescription()
        clients = self.get_client_list()
        if clients:
            self.skipTest('Client list is not empty.')

        # Launch both the client and target instances on the
        # same node.
        procrtn = self.launch_test(testmsg, '1', self.pass_env, \
                                   cli_arg='tests/threaded_client', \
                                   srv_arg='tests/threaded_server')

        if procrtn:
            self.fail("Failed, return code %d" % procrtn)

    def test_threaded_two_nodes(self):
        """Simple threaded test two nodes"""

        if not os.getenv('TR_USE_URI', ""):
            self.skipTest('requires two or more nodes.')

        testmsg = self.shortDescription()

        clients = self.get_client_list()
        if not clients:
            self.skipTest('Client list is empty.')

        servers = self.get_server_list()
        if not servers:
            self.skipTest('Server list is empty.')

        server = ''.join([' -H ', servers.pop(0)])
        # Launch a threaded_server instance to act as a target in the
        # background.  Client will send shutdown when finis
        proc_srv = self.launch_bg(testmsg, '1', self.pass_env, \
                                  server, 'tests/threaded_server')

        if proc_srv is None:
            self.fail("Server launch failed, return code %s" \
                       % proc_srv.returncode)

        time.sleep(1)

        # Just a sanity check to ensure the server hasn't already crashed
        # to avoid running a long test
        if not self.check_process(proc_srv):
            procrtn = self.stop_process(testmsg, proc_srv)
            self.fail("Server did not launch, return code %s" \
                       % procrtn)
        self.logger.info("Server running")

        # Launch, and wait for the test itself.  This is where the actual
        # code gets run.  Launch and keep the return code, but do not check it
        # until after the target has been stopped.
        cli_rtn = self.launch_test(testmsg, '1', self.pass_env, \
                                   cli=''.join([' -H ', clients.pop(0)]), \
                                   cli_arg='tests/threaded_client')

        # Stop the server.
        srv_rtn = self.stop_process(testmsg, proc_srv)

        if cli_rtn or srv_rtn:
            self.fail("Failed, return codes client %d " % cli_rtn + \
                       "server %d" % srv_rtn)
