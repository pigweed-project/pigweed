# Copyright 2025 The Pigweed Authors
#
# Licensed under the Apache License, Version 2.0 (the "License"); you may not
# use this file except in compliance with the License. You may obtain a copy of
# the License at
#
#     https://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS, WITHOUT
# WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied. See the
# License for the specific language governing permissions and limitations under
# the License.
"""An interface for defining Workflows build drivers."""

import abc
from collections.abc import Sequence
import argparse
import json
import sys
import typing

from google.protobuf import any_pb2, descriptor_pb2, message
from google.protobuf import json_format, descriptor, descriptor_pool
from pw_build.proto import build_driver_pb2

MessageT = typing.TypeVar('MessageT', bound=message.Message)


class BuildDriver(abc.ABC):
    """A Workflows build driver interface for drivers written in Python."""

    @abc.abstractmethod
    def generate_action_sequence(
        self, job: build_driver_pb2.JobRequest
    ) -> build_driver_pb2.JobResponse:
        """Generates an action sequence from a single job request."""

    @staticmethod
    def extra_descriptors() -> Sequence[descriptor.FileDescriptor]:
        """Extra descriptors that should be used when loading the proto."""
        return []

    def generate_jobs(
        self, build_request: build_driver_pb2.BuildDriverRequest
    ) -> build_driver_pb2.BuildDriverResponse:
        return build_driver_pb2.BuildDriverResponse(
            jobs=[
                self.generate_action_sequence(job) for job in build_request.jobs
            ]
        )

    def generate_jobs_from_json(
        self, json_msg: str
    ) -> build_driver_pb2.BuildDriverResponse:
        parsed_json = json.loads(json_msg)
        msg = build_driver_pb2.BuildDriverRequest()

        # Load Any proto message:
        pool = descriptor_pool.Default()
        for desc in self.extra_descriptors():
            extension = descriptor_pb2.FileDescriptorProto()
            desc.CopyToProto(extension)
            pool.Add(extension)

        json_format.ParseDict(parsed_json, msg, descriptor_pool=pool)
        return self.generate_jobs(msg)

    @staticmethod
    def unpack_driver_options(
        msg_type: type[MessageT], any_msg: any_pb2.Any
    ) -> MessageT:
        msg = msg_type()
        if not any_msg.value:
            return msg

        desc = msg_type.DESCRIPTOR
        if desc.full_name != any_msg.type_url:
            raise TypeError(
                f'Driver options are of wrong type, expected '
                f'`{desc.full_name}` and got '
                f'`{any_msg.type_url}`'
            )
        msg.ParseFromString(any_msg.value)
        return msg

    @staticmethod
    def _parse_args():
        parser = argparse.ArgumentParser()
        parser.add_argument(
            '--request',
            default=sys.stdin,
            type=argparse.FileType('r'),
        )
        return parser.parse_args()

    def main(self) -> int:
        args = self._parse_args()
        response = self.generate_jobs_from_json(args.request.read())

        print(
            json_format.MessageToJson(
                response,
                preserving_proto_field_name=True,
            )
        )
        return 0
