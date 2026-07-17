// Copyright 2019 The Pigweed Authors
//
// Licensed under the Apache License, Version 2.0 (the "License"); you may not
// use this file except in compliance with the License. You may obtain a copy of
// the License at
//
//	https://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS, WITHOUT
// WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied. See the
// License for the specific language governing permissions and limitations under
// the License.
package main

import (
	"context"
	"errors"
	"flag"
	"fmt"
	"log"
	"os"
	"time"

	"google.golang.org/grpc"
	"google.golang.org/grpc/codes"
	"google.golang.org/grpc/status"

	pb "pigweed/proto/pw_target_runner/target_runner_pb"
)

// Client is a gRPC client that communicates with a TargetRunner service.
type Client struct {
	conn *grpc.ClientConn
}

// NewClient creates a gRPC client which connects to a gRPC server hosted at the
// specified address.
func NewClient(host string, port int) (*Client, error) {
	// The server currently only supports running locally over an insecure
	// connection.
	// TODO(frolv): Investigate adding TLS support to the server and client.
	opts := []grpc.DialOption{
		grpc.WithInsecure(),
		grpc.WithDefaultCallOptions(grpc.MaxCallSendMsgSize(20 * 1024 * 1024)),
	}

	conn, err := grpc.Dial(fmt.Sprintf("%s:%d", host, port), opts...)
	if err != nil {
		return nil, err
	}

	return &Client{conn}, nil
}

// RunBinary sends a RunBinary RPC to the target runner service.
func (c *Client) RunBinary(path string, runner_args []string) error {
	data, err := os.ReadFile(path)
	if err != nil {
		return err
	}

	client := pb.NewTargetRunnerClient(c.conn)
	req := &pb.RunBinaryRequest{Binary: &pb.RunBinaryRequest_TestBinary{TestBinary: data}, RunnerArguments: runner_args}

	res, err := client.RunBinary(context.Background(), req)
	if err != nil {
		return err
	}

	fmt.Printf("%s\n", path)
	fmt.Printf(
		"Queued for %v, ran in %v\n\n",
		time.Duration(res.QueueTimeNs),
		time.Duration(res.RunTimeNs),
	)
	fmt.Println(string(res.Output))

	if res.Result != pb.RunStatus_SUCCESS {
		return errors.New("Binary run was unsuccessful")
	}

	return nil
}

func main() {
	hostPtr := flag.String("host", "localhost", "Server host")
	portPtr := flag.Int("port", 8080, "Server port")
	pathPtr := flag.String("binary", "", "Path to executable file")
	var runnerArgs []string
	flag.Func("runner_arg", "Argument passed to the runner", func(flagValue string) error {
		runnerArgs = append(runnerArgs, flagValue)
		return nil
	})
	serverSuggestionPtr := flag.String("server_suggestion", "", "Suggested command to display if no server is available.")

	flag.Parse()

	if *pathPtr == "" {
		log.Fatalf("Must provide -binary option")
	}

	cli, err := NewClient(*hostPtr, *portPtr)
	if err != nil {
		log.Fatalf("Failed to create gRPC client: %v", err)
	}

	if err := cli.RunBinary(*pathPtr, runnerArgs); err != nil {
		log.Println("Failed to run executable on target:")
		log.Println("")

		s, _ := status.FromError(err)
		if s.Code() == codes.Unavailable {
			log.Println("  No pw_target_runner_server is running.")
			log.Println("  Check that a server has been started for your target.")
			if *serverSuggestionPtr != "" {
				log.Println("  Consider starting a test server with: ", *serverSuggestionPtr)
			}
		} else {
			log.Printf("  %v\n", err)
		}

		log.Fatal("")
	}
}
