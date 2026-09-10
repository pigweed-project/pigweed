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

package dev.pigweed.pw_rpc.examples;

import com.google.common.util.concurrent.FutureCallback;
import com.google.common.util.concurrent.Futures;
import com.google.common.util.concurrent.ListenableFuture;
import com.google.common.util.concurrent.MoreExecutors;
import dev.pigweed.pw_rpc.Call;
import dev.pigweed.pw_rpc.Channel;
import dev.pigweed.pw_rpc.Client;
import dev.pigweed.pw_rpc.MethodClient;
import dev.pigweed.pw_rpc.Service;
import dev.pigweed.pw_rpc.Status;
import dev.pigweed.pw_rpc.StreamObserver;
import dev.pigweed.pw_rpc.UnaryResult;
import java.nio.ByteBuffer;
import java.util.List;

/** Example snippets for pw_rpc Java client documentation. */
public final class DocsExample {
  private DocsExample() {}

  // DOCSTAG: [pw_rpc-java-create-client]
  public static Client createClient(Service sensorService) {
    // 1. Define channel output
    Channel channel = new Channel(1, (byte[] data) -> {
      // Transmit byte buffer over physical transport (USB, Bluetooth, etc.)
      System.out.println("Sending " + data.length + " bytes to device");
    });

    // 2. Instantiate Client
    return Client.createMultiCall(List.of(channel), List.of(sensorService));
  }
  // DOCSTAG: [pw_rpc-java-create-client]

  // DOCSTAG: [pw_rpc-java-route-packets]
  public static void onDataReceived(Client client, byte[] rawPacket) {
    client.processPacket(ByteBuffer.wrap(rawPacket));
  }
  // DOCSTAG: [pw_rpc-java-route-packets]

  // DOCSTAG: [pw_rpc-java-unary-call]
  public static <TReq, TResp> Call callUnary(
      Client client, MethodClient methodClient, TReq request) {
    return methodClient.invokeUnary(request, new StreamObserver<TResp>() {
      @Override
      public void onNext(TResp response) {
        System.out.println("Received response: " + response);
      }

      @Override
      public void onCompleted(Status status) {
        System.out.println("RPC finished with status: " + status);
      }

      @Override
      public void onError(Status status) {
        System.err.println("RPC failed with error: " + status);
      }
    });
  }
  // DOCSTAG: [pw_rpc-java-unary-call]

  // DOCSTAG: [pw_rpc-java-server-streaming-call]
  public static <TReq, TResp> Call callServerStreaming(
      Client client, MethodClient methodClient, TReq request) {
    return methodClient.invokeServerStreaming(request, new StreamObserver<TResp>() {
      @Override
      public void onNext(TResp response) {
        System.out.println("Stream response: " + response);
      }

      @Override
      public void onCompleted(Status status) {
        System.out.println("Stream completed: " + status);
      }

      @Override
      public void onError(Status status) {
        System.err.println("Stream error: " + status);
      }
    });
  }
  // DOCSTAG: [pw_rpc-java-server-streaming-call]

  // DOCSTAG: [pw_rpc-java-streaming-call]
  public static <TReq, TResp> void callBidirectionalStreaming(
      Client client, MethodClient methodClient, TReq chunk1, TReq chunk2) {
    StreamObserver<TResp> observer = new StreamObserver<>() {
      @Override
      public void onNext(TResp response) {
        System.out.println("Stream response: " + response);
      }

      @Override
      public void onCompleted(Status status) {
        System.out.println("Stream finished: " + status);
      }

      @Override
      public void onError(Status status) {
        System.err.println("Stream error: " + status);
      }
    };

    Call.ClientStreaming<TReq> stream = methodClient.invokeBidirectionalStreaming(observer);

    // Stream requests to server
    stream.send(chunk1);
    stream.send(chunk2);

    // Finish sending from client
    stream.finish();
  }
  // DOCSTAG: [pw_rpc-java-streaming-call]

  // DOCSTAG: [pw_rpc-java-future-call]
  public static <TReq, TResp> void callUnaryFuture(
      Client client, MethodClient methodClient, TReq request) {
    ListenableFuture<UnaryResult<TResp>> future = methodClient.invokeUnaryFuture(request);

    // Access result or attach listeners:
    Futures.addCallback(future, new FutureCallback<UnaryResult<TResp>>() {
      @Override
      public void onSuccess(UnaryResult<TResp> result) {
        if (result.status().ok()) {
          System.out.println("Result: " + result.response());
        }
      }

      @Override
      public void onFailure(Throwable t) {
        t.printStackTrace();
      }
    }, MoreExecutors.directExecutor());
  }
  // DOCSTAG: [pw_rpc-java-future-call]
}
