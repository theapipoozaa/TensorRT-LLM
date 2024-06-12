import argparse

template = '''{{
  "num_experts": {num_experts},
  "k": {k},
  "hidden_size": {hidden_size},
  "inter_size": {inter_size},
  "tp_size": {tp_size},
  "ep_size": {ep_size},
  "world_rank": {world_rank},
  "num_tokens": {num_tokens},
  "bias": 0,
  "act_fn": {act_fn},
  "norm_mode": {norm_mode},
  {dtype_string}
  {routing_string}
  "tactic_id": {tactic_id}
}}'''


def make_dtype_string(dtypes=None):
    if dtypes is None:
        return ""
    if not isinstance(dtypes, list):
        dtypes = [dtypes]
    join_term = '","'  # Include quotes because they should be strings
    return f'"dtypes": ["{join_term.join(dtypes)}"],'


def make_routing_string(name=None, values=None):
    if values is None and name is None:
        return ""
    if values is None:
        return f'"routing_values": "{name}",'

    values = f'"routing_values": [{",".join(values)}],'
    if name is not None:
        values += f' "routing_values_name": "{name}",'

    return values


def populate_benchmark_config(**kwargs):
    return template.format(**kwargs)


# Default Mixtral configurations
num_experts = 8
k = 2
hidden_size = 4096
inter_size = 14336
tp_size = 4
ep_size = 1
world_rank = 0
act_fn = 3
norm_mode = 1
dtype_string = make_dtype_string()  # All dtypes
routing_string = make_routing_string(
    name="balanced")  # Use the default uniform distribution
tactic_id = '"auto"'

configs = []
for num_tokens in [1, 8, 64, 2048, 65536]:
    configs.append(
        populate_benchmark_config(
            num_experts=num_experts,
            k=k,
            hidden_size=hidden_size,
            inter_size=inter_size,
            tp_size=tp_size,
            ep_size=ep_size,
            world_rank=world_rank,
            num_tokens=num_tokens,
            act_fn=act_fn,
            norm_mode=norm_mode,
            dtype_string=dtype_string,
            routing_string=routing_string,
            tactic_id=tactic_id,
        ))

full_string = "[\n" + ",\n".join(configs) + "\n]"

parser = argparse.ArgumentParser()
parser.add_argument('filename',
                    type=str,
                    help='The name of the file to generate',
                    nargs='?',
                    default="moe-benchmark-file.json")
args = parser.parse_args()

with open(args.filename, "w+") as f:
    f.write(full_string)
