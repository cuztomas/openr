#!/usr/bin/env python3

#
# Copyright (c) 2014-present, Facebook, Inc.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.
#


import json

#
# Set encoding to UTF-8 for all modules as it is needed for click in python3
#
import locale

#
# Disable click unicode literals warning before importing other modules
#
import click
from openr.cli.clis import (
    config,
    decision,
    fib,
    health_checker,
    kvstore,
    lm,
    monitor,
    perf,
    plugin,
    prefix_mgr,
    tech_support,
)
from openr.cli.utils.options import OPTIONS, breeze_option


click.disable_unicode_literals_warning = True


def getpreferredencoding(do_setlocale=True):
    return "utf-8"


locale.getpreferredencoding = getpreferredencoding


@click.group(name="breeze")
# make host eager (option callback is called before others) sice some default
# options can depend on this
@breeze_option("--host", "-H", help="Host to connect to", is_eager=True)
@breeze_option(
    "--timeout", "-t", type=click.INT, help="Timeout for socket communication in ms"
)
@breeze_option("--color/--no-color", help="Enable coloring display")
@breeze_option("--verbose/--no-verbose", help="Print verbose information")
@click.option(
    "--ports-config-file",
    "-f",
    default=None,
    type=str,
    help="DEPRECATED Perfer setting in openr.cli.utils.default_option_overrides"
    ". JSON file for ports config",
)
@breeze_option("--ssl/--no-ssl", help="Prefer SSL thrift to connect to OpenR")
@breeze_option(
    "--prefer-zmq/--no-prefer-zmq",
    help="Prefer zmq to connect to OpenR. Skip trying to connect "
    "with thrift all together",
)
@breeze_option(
    "--cert-file",
    help="If we are connecting to an SSL server, this points at the "
    "certfile we will present",
)
@breeze_option(
    "--key-file",
    help="If we are connecting to an SSL server, this points at the "
    "keyfile associated with the certificate will present",
)
@breeze_option(
    "--ca-file",
    help="If we are connecting to an SSL server, this points at the "
    "certificate authority we will use to verify peers",
)
@breeze_option(
    "--acceptable-peer-name",
    help="If we are connecting to an SSL server, this is the common "
    "name we deem acceptable to connect to.",
)
@click.pass_context
def cli(
    ctx,
    host,
    timeout,
    ports_config_file,
    color,
    ssl,
    prefer_zmq,
    cert_file,
    key_file,
    ca_file,
    acceptable_peer_name,
    verbose,
):
    """ Command line tools for Open/R. """

    # Default config options
    ctx.obj = OPTIONS

    # Get override port configs
    if ports_config_file:
        with open(ports_config_file, "r") as f:
            override_ports_config = json.load(f)
            for key, value in override_ports_config.items():
                ctx.obj[key] = value


def get_breeze_cli():

    # add cli submodules
    cli.add_command(config.ConfigCli().config)
    cli.add_command(decision.DecisionCli().decision)
    cli.add_command(fib.FibCli().fib)
    cli.add_command(health_checker.HealthCheckerCli().healthchecker)
    cli.add_command(kvstore.KvStoreCli().kvstore)
    cli.add_command(lm.LMCli().lm)
    cli.add_command(monitor.MonitorCli().monitor)
    cli.add_command(perf.PerfCli().perf)
    cli.add_command(prefix_mgr.PrefixMgrCli().prefixmgr)
    cli.add_command(tech_support.TechSupportCli().tech_support)
    plugin.plugin_start(cli)
    return cli


def main():
    """ entry point for breeze """

    # let the magic begin
    cli = get_breeze_cli()
    cli()


if __name__ == "__main__":
    main()
