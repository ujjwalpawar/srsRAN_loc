#!/usr/bin/env python3
#
# Simple helper that sends a positioning_request command to one or more DU
# remote-control endpoints. Requires the "websockets" package
# (pip install websockets).
#

import argparse
import asyncio
import json
from pathlib import Path
from typing import List, Union

import websockets


def _wrap_cells(cell_payload: Union[dict, List[dict]]) -> dict:
    """
    Normalizes different descriptor shapes into the remote-command schema.
    Supported inputs:
      * {"cmd": "...", "cells": [...]} -> passed through unchanged.
      * {"cells": [...]} -> wraps with cmd.
      * {"plmn": ..., "nci": ..., "schedule": {...}} -> wrapped in single-cell list.
      * [ {... cell entries ...} ] -> wrapped with cmd.
    """
    if isinstance(cell_payload, dict) and cell_payload.get("cmd"):
        return cell_payload

    if isinstance(cell_payload, dict) and "cells" in cell_payload:
        return {"cmd": "positioning_request", "cells": cell_payload["cells"]}

    if isinstance(cell_payload, list):
        return {"cmd": "positioning_request", "cells": cell_payload}

    return {"cmd": "positioning_request", "cells": [cell_payload]}


def build_payload(args: argparse.Namespace) -> str:
    if args.schedule_file:
        descriptor = json.loads(Path(args.schedule_file).read_text())
        payload = _wrap_cells(descriptor)
        return json.dumps(payload)

    resource = {
        "cell_res_id": args.cell_res_id,
        "ue_res_id": args.ue_res_id,
        "nof_ports": args.nof_ports,
        "res_mapping": {
            "start_symbol": args.start_symbol,
            "nof_symbols": args.nof_symbols,
            "repetition_factor": args.repetition_factor,
        },
        "freq_domain_pos": args.freq_domain_pos,
        "freq_domain_shift": args.freq_domain_shift,
        "freq_hop": {"b_srs": args.b_srs, "b_hop": args.b_hop, "c_srs": args.c_srs},
        "tx_comb": {
            "size": args.tx_comb_size,
            "offset": args.tx_comb_offset,
            "cyclic_shift": args.tx_comb_cyclic_shift,
        },
        "sequence_id": args.sequence_id,
        "group_or_sequence_hopping": args.group_or_sequence_hopping,
        "resource_type": args.resource_type,
        "periodicity": {"t_srs": args.t_srs, "offset": args.t_offset},
    }

    payload = {
        "cmd": "positioning_request",
        "cells": [
            {
                "plmn": args.plmn,
                "nci": args.nci,
                "schedule": {
                    "imeisv": args.imeisv,
                    "rnti": args.rnti,
                    "resource": resource,
                },
            }
        ],
    }

    return json.dumps(payload)


async def send_request(url: str, payload: str) -> None:
    async with websockets.connect(url) as ws:
        await ws.send(payload)
        response = await ws.recv()
        print(f"{url}: {response}")


async def main(urls: List[str], payload: str) -> None:
    await asyncio.gather(*(send_request(url, payload) for url in urls))


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description="Trigger positioning_request on DUs via remote control.")
    parser.add_argument(
        "--du",
        action="append",
        required=True,
        help="Remote-control URL of DU (e.g. ws://10.0.0.5:5555/). Can be repeated.",
    )
    parser.add_argument(
        "--schedule-file",
        help="Path to JSON descriptor emitted by the serving gNB. "
        "Accepts either a full positioning_request payload or a list/dict of cell entries.",
    )
    parser.add_argument("--plmn", help="PLMN string (manual mode).")
    parser.add_argument("--nci", type=int, help="NR Cell Identity (manual mode).")
    parser.add_argument("--imeisv", help="Target UE IMEISV (manual mode).")
    parser.add_argument("--rnti", type=lambda x: int(x, 0), help="Target RNTI (manual mode).")

    parser.add_argument("--cell-res-id", type=int, default=0)
    parser.add_argument("--ue-res-id", type=int, default=0)
    parser.add_argument("--nof-ports", type=int, default=1)
    parser.add_argument("--start-symbol", type=int, default=10)
    parser.add_argument("--nof-symbols", type=int, default=1)
    parser.add_argument("--repetition-factor", type=int, default=1)
    parser.add_argument("--freq-domain-pos", type=int, default=0)
    parser.add_argument("--freq-domain-shift", type=int, default=0)
    parser.add_argument("--b-srs", type=int, default=0)
    parser.add_argument("--b-hop", type=int, default=0)
    parser.add_argument("--c-srs", type=int, default=0)
    parser.add_argument("--tx-comb-size", type=int, default=2)
    parser.add_argument("--tx-comb-offset", type=int, default=0)
    parser.add_argument("--tx-comb-cyclic-shift", type=int, default=0)
    parser.add_argument("--sequence-id", type=int, default=0)
    parser.add_argument(
        "--group-or-sequence-hopping",
        choices=["neither", "group", "sequence"],
        default="neither",
    )
    parser.add_argument(
        "--resource-type",
        choices=["periodic", "semi_persistent", "aperiodic"],
        default="periodic",
    )
    parser.add_argument("--t-srs", type=int, default=20)
    parser.add_argument("--t-offset", type=int, default=0)

    args = parser.parse_args()
    if not args.schedule_file:
        required = ["plmn", "nci", "imeisv", "rnti"]
        missing = [field for field in required if getattr(args, field) is None]
        if missing:
            parser.error(f"Missing required arguments in manual mode: {', '.join(missing)}")

    asyncio.run(main(args.du, build_payload(args)))
