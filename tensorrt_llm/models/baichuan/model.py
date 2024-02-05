# SPDX-FileCopyrightText: Copyright (c) 2022-2024 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
# SPDX-License-Identifier: Apache-2.0
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
# http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.
from ..._utils import pad_vocab_size
from ...functional import Tensor
from ...layers import (Attention, AttentionMaskType, ColumnLinear, Embedding,
                       GatedMLP, PromptTuningEmbedding, RmsNorm)
from ...module import Module
from ..modeling_utils import (DecoderLayerList, DecoderModelForCausalLM,
                              PretrainedConfig)


class BaichuanDecoderLayer(Module):

    def __init__(self, config: PretrainedConfig, layer_idx):
        super().__init__()
        self.layer_idx = layer_idx
        self.config = config
        hidden_size = config.hidden_size
        dtype = config.dtype
        position_embedding_type = config.position_embedding_type
        tp_group = config.mapping.tp_group
        tp_size = config.mapping.tp_size
        tp_rank = config.mapping.tp_rank
        quant_mode = config.quant_mode

        self.input_layernorm = RmsNorm(normalized_shape=hidden_size,
                                       dtype=dtype)

        self.attention = Attention(
            hidden_size,
            config.num_attention_heads,
            max_position_embeddings=config.max_position_embeddings,
            dtype=dtype,
            attention_mask_type=AttentionMaskType.causal,
            bias=False,
            position_embedding_type=position_embedding_type,
            tp_group=tp_group,
            tp_size=tp_size,
            tp_rank=tp_rank,
            quant_mode=quant_mode)

        self.mlp = GatedMLP(hidden_size=hidden_size,
                            ffn_hidden_size=config.intermediate_size,
                            hidden_act=config.hidden_act,
                            dtype=dtype,
                            bias=False,
                            tp_group=tp_group,
                            tp_size=tp_size,
                            quant_mode=quant_mode)
        self.post_layernorm = RmsNorm(normalized_shape=hidden_size, dtype=dtype)

    def forward(self,
                hidden_states: Tensor,
                attention_mask=None,
                use_cache=False,
                kv_cache_params=None,
                attention_params=None):
        residual = hidden_states
        hidden_states = self.input_layernorm(hidden_states)

        attention_output = self.attention(hidden_states,
                                          attention_mask=attention_mask,
                                          use_cache=use_cache,
                                          kv_cache_params=kv_cache_params,
                                          attention_params=attention_params)

        if use_cache:
            attention_output, presents = attention_output

        hidden_states = residual + attention_output

        residual = hidden_states
        hidden_states = self.post_layernorm(hidden_states)

        hidden_states = self.mlp(hidden_states)

        hidden_states = residual + hidden_states
        if use_cache:
            return (hidden_states, presents)
        return hidden_states


class BaichuanModel(Module):

    def __init__(self, config: PretrainedConfig):
        super().__init__()
        hidden_size = config.hidden_size
        dtype = config.dtype
        self.use_prompt_tuning = config.use_prompt_tuning

        EmbeddingCls = PromptTuningEmbedding if config.use_prompt_tuning else Embedding
        self.vocab_embedding = EmbeddingCls(
            config.vocab_size,
            config.hidden_size,
            dtype=config.dtype,
            tp_size=config.mapping.tp_size
            if config.use_parallel_embedding else 1,
            tp_group=config.mapping.tp_group
            if config.use_parallel_embedding else None,
            sharding_dim=config.embedding_sharding_dim,
            tp_rank=config.mapping.tp_rank)

        self.layers = DecoderLayerList(BaichuanDecoderLayer, config)
        self.ln_f = RmsNorm(normalized_shape=hidden_size, dtype=dtype)

    def forward(self,
                input_ids: Tensor,
                position_ids=None,
                use_cache=False,
                attention_mask=None,
                kv_cache_params=None,
                attention_params=None,
                prompt_embedding_table=None,
                prompt_tasks=None,
                prompt_vocab_size=None):
        args = [prompt_embedding_table, prompt_tasks, prompt_vocab_size
                ] if self.use_prompt_tuning else []
        hidden_states = self.vocab_embedding(input_ids, *args)

        hidden_states = self.layers(hidden_states,
                                    use_cache=use_cache,
                                    attention_mask=attention_mask,
                                    kv_cache_params=kv_cache_params,
                                    attention_params=attention_params)

        if use_cache:
            hidden_states, presents = hidden_states

        hidden_states = self.ln_f(hidden_states)

        if use_cache:
            return (hidden_states, tuple(presents))
        return hidden_states


class BaichuanForCausalLM(DecoderModelForCausalLM):

    def __init__(self, config: PretrainedConfig):
        transformer = BaichuanModel(config)
        vocab_size_padded = pad_vocab_size(config.vocab_size,
                                           config.mapping.tp_size)
        lm_head = ColumnLinear(config.hidden_size,
                               vocab_size_padded,
                               bias=False,
                               dtype=config.dtype,
                               tp_group=config.mapping.tp_group,
                               tp_size=config.mapping.tp_size,
                               gather_output=True)
        super().__init__(config, transformer, lm_head)
