// Copyright 2022 The Pigweed Authors
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

package dev.pigweed.pw_transfer;

import com.google.common.util.concurrent.ListenableFuture;
import dev.pigweed.pw_log.Logger;
import dev.pigweed.pw_rpc.Call;
import dev.pigweed.pw_rpc.ChannelOutputException;
import dev.pigweed.pw_rpc.MethodClient;
import dev.pigweed.pw_rpc.Status;
import dev.pigweed.pw_rpc.StreamObserver;
import java.time.Instant;
import java.time.temporal.ChronoUnit;
import java.time.temporal.TemporalUnit;
import java.util.Comparator;
import java.util.HashMap;
import java.util.Map;
import java.util.Optional;
import java.util.concurrent.BlockingQueue;
import java.util.concurrent.LinkedBlockingQueue;
import java.util.concurrent.Semaphore;
import java.util.concurrent.TimeUnit;
import java.util.function.BooleanSupplier;
import java.util.function.Consumer;
import javax.annotation.Nullable;

/** Manages the active transfers and dispatches events to them. */
class TransferEventHandler {
  private static final Logger logger = Logger.forClass(TransferEventHandler.class);

  // Instant and BlockingQueue use different time unit types.
  private static final TemporalUnit TIME_UNIT = ChronoUnit.MICROS;
  private static final TimeUnit POLL_TIME_UNIT = TimeUnit.MICROSECONDS;

  private final MethodClient readMethod;
  private final MethodClient writeMethod;

  private final BlockingQueue<Event> events = new LinkedBlockingQueue<>();

  // Map session ID to transfer.
  private final Map<Integer, Transfer<?>> sessionIdToTransfer = new HashMap<>();
  // Legacy transfers only use the resource ID. The client assigns an arbitrary session ID that
  // legacy servers ignore. The client then maps from the legacy ID to its local session ID.
  private final Map<Integer, Integer> legacyIdToSessionId = new HashMap<>();

  @Nullable private Call.ClientStreaming<Chunk> readStream = null;
  @Nullable private Call.ClientStreaming<Chunk> writeStream = null;
  private boolean processEvents = true;

  private int nextSessionId = 1;

  TransferEventHandler(MethodClient readMethod, MethodClient writeMethod) {
    this.readMethod = readMethod;
    this.writeMethod = writeMethod;
  }

  ListenableFuture<Void> startWriteTransferAsClient(int resourceId,
      ProtocolVersion desiredProtocolVersion,
      TransferTimeoutSettings settings,
      byte[] data,
      Consumer<TransferProgress> progressCallback,
      BooleanSupplier shouldAbortCallback,
      int initialOffset) {
    WriteTransfer transfer = new WriteTransfer(
        resourceId, assignSessionId(), desiredProtocolVersion, new TransferInterface() {
          @Override
          Call.ClientStreaming<Chunk> getStream() throws ChannelOutputException {
            if (writeStream == null) {
              writeStream = writeMethod.invokeBidirectionalStreaming(new ChunkHandler() {
                @Override
                void resetStream() {
                  writeStream = null;
                }
              });
              // TODO(b/355249134): Revert this sleep workaround when transfer
              // sets on_next_ sooner
              try {
                Thread.sleep(10);
              } catch (InterruptedException e) {
                logger.atWarning().log("Interrupted while waiting for write stream");
              }
            }
            return writeStream;
          }
        }, settings, data, progressCallback, shouldAbortCallback, initialOffset);
    startTransferAsClient(transfer);
    return transfer;
  }

  ListenableFuture<byte[]> startReadTransferAsClient(int resourceId,
      ProtocolVersion desiredProtocolVersion,
      TransferTimeoutSettings settings,
      TransferParameters parameters,
      Consumer<TransferProgress> progressCallback,
      BooleanSupplier shouldAbortCallback,
      int initialOffset) {
    ReadTransfer transfer = new ReadTransfer(
        resourceId, assignSessionId(), desiredProtocolVersion, new TransferInterface() {
          @Override
          Call.ClientStreaming<Chunk> getStream() throws ChannelOutputException {
            if (readStream == null) {
              readStream = readMethod.invokeBidirectionalStreaming(new ChunkHandler() {
                @Override
                void resetStream() {
                  readStream = null;
                }
              });
              // TODO(b/355249134): Revert this sleep workaround when transfer
              // sets on_next_ sooner
              try {
                Thread.sleep(10);
              } catch (InterruptedException e) {
                logger.atWarning().log("Interrupted while waiting for read stream");
              }
            }
            return readStream;
          }
        }, settings, parameters, progressCallback, shouldAbortCallback, initialOffset);
    startTransferAsClient(transfer);
    return transfer;
  }

  private void startTransferAsClient(Transfer<?> transfer) {
    enqueueEvent(() -> {
      if (sessionIdToTransfer.containsKey(transfer.getSessionId())) {
        throw new AssertionError("Duplicate session ID " + transfer.getSessionId());
      }

      if (transfer.getDesiredProtocolVersion() == ProtocolVersion.LEGACY
          && transfer.getOffset() != 0) {
        throw new AssertionError("Cannot start non-zero offset transfer with legacy version");
      }

      // The v2 protocol supports multiple transfers for a single resource. For simplicity while
      // supporting both protocols, only support a single transfer per resource.
      if (legacyIdToSessionId.containsKey(transfer.getResourceId())) {
        transfer.terminate(
            new TransferError("A transfer for resource ID " + transfer.getResourceId()
                    + " is already in progress! Only one read/write transfer per resource is "
                    + "supported at a time",
                Status.ALREADY_EXISTS));
        return;
      }
      sessionIdToTransfer.put(transfer.getSessionId(), transfer);
      legacyIdToSessionId.put(transfer.getResourceId(), transfer.getSessionId());
      transfer.start();
    });
  }

  /** Handles events until stop() is called. */
  void run() {
    while (processEvents) {
      handleNextEvent();
      handleTimeouts();
    }
  }

  /**
   * Test version of run() that processes all enqueued events before checking for timeouts.
   *
   * Tests that need to time out should process all enqueued events first to prevent flaky failures.
   * If handling one of several queued packets takes longer than the timeout (which must be short
   * for a unit test), then the test may fail spuriously.
   *
   * This run function is not used outside of tests because processing all incoming packets before
   * checking for timeouts could delay the transfer client's outgoing write packets if there are
   * lots of inbound packets. This could delay transfers and cause unnecessary timeouts.
   */
  void runForTestsThatMustTimeOut() {
    while (processEvents) {
      do {
        handleNextEvent();
      } while (processEvents && !events.isEmpty());
      handleTimeouts();
    }
  }

  /** Stops the transfer event handler from processing events. */
  void stop() {
    enqueueEvent(() -> {
      logger.atFine().log("Terminating TransferEventHandler");
      for (Transfer<?> transfer : sessionIdToTransfer.values().toArray(Transfer<?>[] ::new)) {
        transfer.handleTermination();
      }
      processEvents = false;
    });
  }

  /** Blocks until all events currently in the queue are processed; for test use only. */
  void waitUntilEventsAreProcessedForTest() {
    Semaphore semaphore = new Semaphore(0);
    Event releaseWhenDone = new Event() {
      @Override
      public void handle() {
        handleTimeouts();
        if (events.isEmpty() && Instant.now().isBefore(getNextTimeout())) {
          semaphore.release();
        } else {
          enqueueEvent(this);
        }
      }
    };
    enqueueEvent(releaseWhenDone);
    try {
      semaphore.acquire();
    } catch (InterruptedException e) {
      throw new AssertionError("Unexpectedly interrupted", e);
    }
  }

  /** Generates the session ID to use for the next transfer. */
  private int assignSessionId() {
    return nextSessionId++;
  }

  /** Returns the session ID that will be used for the next transfer. */
  final int getNextSessionIdForTest() {
    return nextSessionId;
  }

  private void enqueueEvent(Event event) {
    while (true) {
      try {
        events.put(event);
        return;
      } catch (InterruptedException e) {
        // Ignore and keep trying.
      }
    }
  }

  private void handleNextEvent() {
    final long sleepFor = TIME_UNIT.between(Instant.now(), getNextTimeout());
    try {
      Event event = events.poll(sleepFor, POLL_TIME_UNIT);
      if (event != null) {
        event.handle();
      }
    } catch (InterruptedException e) {
      // If interrupted, continue around the loop.
    }
  }

  private void handleTimeouts() {
    // Copy to array since transfers may remove themselves from sessionIdToTransfer while iterating.
    for (Transfer<?> transfer : sessionIdToTransfer.values().toArray(Transfer<?>[] ::new)) {
      transfer.handleTimeoutIfDeadlineExceeded();
    }
  }

  private Instant getNextTimeout() {
    Optional<Transfer<?>> transfer =
        sessionIdToTransfer.values().stream().min(Comparator.comparing(Transfer::getDeadline));
    return transfer.isPresent() ? transfer.get().getDeadline() : Transfer.NO_TIMEOUT;
  }

  /** This interface gives a Transfer access to the TransferEventHandler. */
  abstract class TransferInterface {
    private TransferInterface() {}

    /**
     *  Sends the provided transfer chunk.
     *
     *  Must be called on the transfer thread.
     */
    void sendChunk(Chunk chunk) throws TransferError {
      try {
        getStream().write(chunk);
      } catch (ChannelOutputException e) {
        throw new TransferError("Failed to send chunk for write transfer", e);
      }
    }

    /**
     *  Removes this transfer from the list of active transfers.
     *
     *  Must be called on the transfer thread.
     */
    // TODO(frolv): Investigate why this is occurring -- there shouldn't be any
    // futures here.
    @SuppressWarnings("FutureReturnValueIgnored")
    void unregisterTransfer(Transfer<?> transfer) {
      sessionIdToTransfer.remove(transfer.getSessionId());
      legacyIdToSessionId.remove(transfer.getResourceId());
    }

    /**
     * Initiates the cancellation process for the provided transfer.
     *
     * May be called from any thread.
     */
    void cancelTransfer(Transfer<?> transfer) {
      enqueueEvent(transfer::handleCancellation);
    }

    /** Gets either the read or write stream. */
    abstract Call.ClientStreaming<Chunk> getStream() throws ChannelOutputException;
  }

  /** Handles responses on the pw_transfer RPCs. */
  private abstract class ChunkHandler implements StreamObserver<Chunk> {
    @Override
    public final void onNext(Chunk chunkProto) {
      enqueueEvent(() -> {
        VersionedChunk chunk = VersionedChunk.fromMessage(chunkProto, legacyIdToSessionId);

        Transfer<?> transfer;
        if (chunk.sessionId() == VersionedChunk.UNKNOWN_SESSION_ID
            || (transfer = sessionIdToTransfer.get(chunk.sessionId())) == null) {
          logger.atInfo().log("Ignoring unrecognized transfer chunk: %s", chunk);
          return;
        }

        logger.atFinest().log("%s received chunk: %s", transfer, chunk);
        transfer.handleChunk(chunk);
      });
    }

    @Override
    public final void onCompleted(Status status) {
      onError(Status.INTERNAL); // This RPC should never complete: treat as an internal error.
    }

    @Override
    public final void onError(Status status) {
      enqueueEvent(() -> {
        resetStream();

        TransferError error = new TransferError(
            "Transfer stream RPC closed unexpectedly with status " + status, Status.INTERNAL);

        // The transfers remove themselves from the Map during cleanup; iterate over a copied list.
        for (Transfer<?> transfer : sessionIdToTransfer.values().toArray(Transfer<?>[] ::new)) {
          transfer.terminate(error);
        }
      });
    }

    abstract void resetStream();
  }

  // Represents an event that occurs during a transfer
  private interface Event {
    void handle();
  }
}
