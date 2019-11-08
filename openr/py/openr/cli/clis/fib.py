#!/usr/bin/env python3

#
# Copyright (c) 2014-present, Facebook, Inc.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.
#


import sys
from builtins import object
from typing import List

import click
from bunch import Bunch
from openr.cli.commands import fib
from openr.cli.utils.options import breeze_option


class FibCli(object):
    def __init__(self):
        self.fib.add_command(FibRoutesComputedCli().routes, name="routes-computed")
        self.fib.add_command(FibRoutesInstalledCli().routes, name="routes-installed")

        # NOTE: keeping alias `list` and `routes`
        # for backward compatibility. Deprecated.
        self.fib.add_command(FibRoutesComputedCli().routes, name="routes")
        self.fib.add_command(FibRoutesInstalledCli().routes, name="list")

        self.fib.add_command(FibCountersCli().counters, name="counters")
        self.fib.add_command(FibAddRoutesCli().add_routes, name="add")
        self.fib.add_command(FibDelRoutesCli().del_routes, name="del")
        self.fib.add_command(FibSyncRoutesCli().sync_routes, name="sync")
        self.fib.add_command(FibValidateRoutesCli().validate)

    @click.group()
    @breeze_option("--fib_agent_port", type=int, help="Fib thrift server port")
    @breeze_option("--client-id", type=int, help="FIB Client ID")
    @click.pass_context
    def fib(ctx, fib_agent_port, client_id):  # noqa: B902
        """ CLI tool to peek into Fib module. """
        pass


class FibCountersCli(object):
    @click.command()
    @click.option("--json/--no-json", default=False, help="Dump in JSON format")
    @click.pass_obj
    def counters(cli_opts, json):  # noqa: B902
        """ Get various counters on fib agent """

        return_code = fib.FibCountersCmd(cli_opts).run(json)
        sys.exit(return_code)


class FibRoutesInstalledCli(object):
    @click.command()
    @click.option(
        "--prefixes",
        "-p",
        type=click.STRING,
        multiple=True,
        help="Get route for specific IPs or Prefixes.",
    )
    @click.option(
        "--labels",
        "-l",
        type=click.INT,
        multiple=True,
        help="Get route for specific labels.",
    )
    @click.option(
        "--client-id",
        type=click.INT,
        help="Retrieve routes for client. Defaults to 786 (Open/R). Use 0 for BGP",
    )
    @click.option("--json/--no-json", default=False, help="Dump in JSON format")
    @click.pass_obj
    def routes(
        cli_opts: Bunch,  # noqa: B902
        prefixes: List[str],
        labels: List[int],
        client_id: int,
        json: bool,
    ):
        """ Get and print all the routes on fib agent """

        return_code = fib.FibRoutesInstalledCmd(cli_opts).run(
            prefixes, labels, json, client_id
        )
        sys.exit(return_code)


class FibRoutesComputedCli(object):
    @click.command()
    @click.option(
        "--prefixes",
        "-p",
        default="",
        multiple=True,
        help="Get route for specific IPs or Prefixes.",
    )
    @click.option(
        "--labels",
        "-l",
        type=click.INT,
        multiple=True,
        help="Get route for specific labels.",
    )
    @click.option("--json/--no-json", default=False, help="Dump in JSON format")
    @click.pass_obj
    def routes(cli_opts, prefixes, labels, json):  # noqa: B902
        """ Request routing table of the current host """

        fib.FibRoutesComputedCmd(cli_opts).run(prefixes, labels, json)


class FibAddRoutesCli(object):
    @click.command()
    @click.argument("prefixes")  # Comma separated list of prefixes
    @click.argument("nexthops")  # Comma separated list of nexthops
    @click.pass_obj
    def add_routes(cli_opts, prefixes, nexthops):  # noqa: B902
        """ Add new routes in FIB """

        fib.FibAddRoutesCmd(cli_opts).run(prefixes, nexthops)


class FibDelRoutesCli(object):
    @click.command()
    @click.argument("prefixes")  # Comma separated list of prefixes
    @click.pass_obj
    def del_routes(cli_opts, prefixes):  # noqa: B902
        """ Delete routes from FIB """

        fib.FibDelRoutesCmd(cli_opts).run(prefixes)


class FibSyncRoutesCli(object):
    @click.command()
    @click.argument("prefixes")  # Comma separated list of prefixes
    @click.argument("nexthops")  # Comma separated list of nexthops
    @click.pass_obj
    def sync_routes(cli_opts, prefixes, nexthops):  # noqa: B902
        """ Re-program FIB with specified routes. Delete all old ones """

        fib.FibSyncRoutesCmd(cli_opts).run(prefixes, nexthops)


class FibValidateRoutesCli(object):
    @click.command()
    @click.pass_obj
    def validate(cli_opts):  # noqa: B902
        """ Validator to check that all routes as computed by Decision """

        sys.exit(fib.FibValidateRoutesCmd(cli_opts).run(cli_opts))
