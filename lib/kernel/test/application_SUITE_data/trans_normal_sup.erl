%%
%% %CopyrightBegin%
%%
%% SPDX-License-Identifier: Apache-2.0
%%
%% Copyright Ericsson AB 1998-2025. All Rights Reserved.
%%
%% Licensed under the Apache License, Version 2.0 (the "License");
%% you may not use this file except in compliance with the License.
%% You may obtain a copy of the License at
%%
%%     http://www.apache.org/licenses/LICENSE-2.0
%%
%% Unless required by applicable law or agreed to in writing, software
%% distributed under the License is distributed on an "AS IS" BASIS,
%% WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
%% See the License for the specific language governing permissions and
%% limitations under the License.
%%
%% %CopyrightEnd%
%%
-module(trans_normal_sup).

-behaviour(supervisor).

%% External exports
-export([start/2]).

%% Internal exports
-export([init/1]).

start(_, _) ->
    supervisor:start_link({local, trans_normal_sup}, trans_normal_sup, []),
    exit(normal).

init([]) ->
    SupFlags = {one_for_one, 4, 3600},
    Config = {transient,
	      {transient, start_link, []},
	      transient, 2000, worker, [transient]},
    {ok, {SupFlags, [Config]}}.
