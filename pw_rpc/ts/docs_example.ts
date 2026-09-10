// Copyright 2026 The Pigweed Authors
//
// Licensed under the Apache License, Version 2.0 (the "License"); you may not
// use this file except in compliance with the License. You may obtain a copy of
// the License at
//
//     https://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS, WITHOUT
// WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied. See the
// License for the specific language governing permissions and limitations under
// the License.

import { Message } from 'google-protobuf';
import { Status } from 'pigweedjs/pw_status';
import { ProtoCollection } from 'pigweedjs/protos/collection';
import { RpcPacket } from 'pigweedjs/protos/pw_rpc/internal/packet_pb';
import {
  BidirectionalStreamingMethodStub,
  Channel,
  Client,
  ClientStreamingMethodStub,
  ServerStreamingMethodStub,
  UnaryMethodStub,
} from './index';

// DOCSTAG: [pw_rpc-ts-create-client]
function savePacket(packetBytes: Uint8Array): void {
  const packet = RpcPacket.deserializeBinary(packetBytes);
  // Forward packet to physical transport:
  // transport.send(packet.serializeBinary());
  console.log(`Sending packet to channel ${packet.getChannelId()}`);
}

const channels = [new Channel(1, savePacket), new Channel(5)];
const client = Client.fromProtoSet(channels, new ProtoCollection());
// DOCSTAG: [pw_rpc-ts-create-client]

// DOCSTAG: [pw_rpc-ts-find-method]
const channel = client.channel()!;
const unaryStub = channel.methodStub(
  'pw.rpc.test1.TheTestService.SomeUnary',
) as UnaryMethodStub;
// DOCSTAG: [pw_rpc-ts-find-method]

// DOCSTAG: [pw_rpc-ts-callback-invocation]
const bidiStub = client
  .channel()!
  .methodStub(
    'pw.rpc.test1.TheTestService.SomeBidi',
  ) as BidirectionalStreamingMethodStub;

// Configure callback functions:
const onNext = (response: Message) => {
  console.log(`Received message: ${response}`);
};
const onComplete = (status: Status) => {
  console.log(`RPC completed with status: ${status}`);
};
const onError = (error: Error) => {
  console.error(`RPC error: ${error}`);
};

const request = new bidiStub.method.requestType();
bidiStub.invoke(request, onNext, onComplete, onError);
// DOCSTAG: [pw_rpc-ts-callback-invocation]

// DOCSTAG: [pw_rpc-ts-unary-promise]
async function callUnary(): Promise<void> {
  const unaryRpc = client
    .channel()!
    .methodStub('pw.rpc.test1.TheTestService.SomeUnary') as UnaryMethodStub;

  const req = new unaryRpc.method.requestType();
  const timeoutMs = 2000;
  const [status, response] = await unaryRpc.call(req, timeoutMs);
  console.log(`Status: ${status}, Response: ${response}`);
}
// DOCSTAG: [pw_rpc-ts-unary-promise]

// DOCSTAG: [pw_rpc-ts-server-streaming-promise]
async function callServerStreaming(): Promise<void> {
  const serverStreamRpc = client
    .channel()!
    .methodStub(
      'pw.rpc.test1.TheTestService.SomeServerStreaming',
    ) as ServerStreamingMethodStub;

  const req = new serverStreamRpc.method.requestType();
  const call = serverStreamRpc.invoke(req);
  const timeoutMs = 2000;

  // Stream responses as they arrive:
  for await (const response of call.getResponses(2, timeoutMs)) {
    console.log(response);
  }

  // Await remaining responses until stream completion:
  const responses = call.getResponses();
  while (!responses.done) {
    console.log(await responses.value());
  }
}
// DOCSTAG: [pw_rpc-ts-server-streaming-promise]

// DOCSTAG: [pw_rpc-ts-client-streaming-promise]
async function callClientStreaming(): Promise<void> {
  const clientStreamRpc = client
    .channel()!
    .methodStub(
      'pw.rpc.test1.TheTestService.SomeClientStreaming',
    ) as ClientStreamingMethodStub;

  const stream = clientStreamRpc.invoke();
  const req = new clientStreamRpc.method.requestType();

  // Send request messages to the stream:
  stream.send(req);

  // Complete stream and await final unary response:
  const timeoutMs = 2000;
  stream
    .finishAndWait([req, req], timeoutMs)
    .then(([status, response]) => {
      console.log(`Finished: ${status}, Response: ${response}`);
    })
    .catch((reason) => {
      console.error(`Stream error: ${reason}`);
    });
}
// DOCSTAG: [pw_rpc-ts-client-streaming-promise]

// DOCSTAG: [pw_rpc-ts-bidi-streaming-promise]
async function callBidiStreaming(): Promise<void> {
  const bidiStreamingRpc = client
    .channel()!
    .methodStub(
      'pw.rpc.test1.TheTestService.SomeBidiStreaming',
    ) as BidirectionalStreamingMethodStub;

  const stream = bidiStreamingRpc.invoke();
  const req = new bidiStreamingRpc.method.requestType();

  // Send requests to device:
  stream.send(req);

  // Receive stream responses:
  const timeoutMs = 2000;
  for await (const response of stream.getResponses(1, timeoutMs)) {
    console.log(response);
  }

  // Finish sending and await completion:
  stream
    .finishAndWait([req], timeoutMs)
    .then(([status]) => {
      console.log(`Bidirectional stream finished with status: ${status}`);
    })
    .catch((reason) => {
      console.error(`Bidirectional stream error: ${reason}`);
    });
}
// DOCSTAG: [pw_rpc-ts-bidi-streaming-promise]
