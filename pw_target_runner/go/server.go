// Copyright 2019 The Pigweed Authors
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

// Package pw_target_runner implements a target runner gRPC server which queues
// and distributes executables among a group of worker routines.
package pw_target_runner

import (
	"context"
	"errors"
	"fmt"
	"log"
	"net"
	"os"
	"time"

	"google.golang.org/grpc"
	"google.golang.org/grpc/codes"
	"google.golang.org/grpc/status"

	pb "pigweed/proto/pw_target_runner/target_runner_pb"
)

var (
	errServerNotBound   = errors.New("Server not bound to a port")
	errServerNotRunning = errors.New("Server is not running")
)

// Server is a gRPC server that runs a TargetRunner service.
type Server struct {
	grpcServer  *grpc.Server
	listener    net.Listener
	tasksPassed uint32
	tasksFailed uint32
	startTime   time.Time
	active      bool
	workerPool  *WorkerPool
}

// NewServer creates a gRPC server with a registered TargetRunner service.
func NewServer() *Server {
	s := &Server{
		grpcServer: grpc.NewServer(grpc.MaxRecvMsgSize(20 * 1024 * 1024)),
		workerPool: newWorkerPool("ServerWorkerPool"),
	}

	pb.RegisterTargetRunnerServer(s.grpcServer, &pwTargetRunnerService{s})

	return s
}

// Bind starts a TCP listener on a specified port.
func (s *Server) Bind(port int) error {
	lis, err := net.Listen("tcp", fmt.Sprintf(":%d", port))
	if err != nil {
		return err
	}
	s.listener = lis
	return nil
}

// RegisterWorker adds a worker to the server's worker pool.
func (s *Server) RegisterWorker(worker DeviceRunner) {
	s.workerPool.RegisterWorker(worker)
}

// RunBinary runs an executable through a worker in the server, returning
// the worker's response. The function blocks until the executable has been
// processed.
func (s *Server) RunBinary(path string, runner_args []string) (*RunResponse, error) {
	if !s.active {
		return nil, errServerNotRunning
	}

	resChan := make(chan *RunResponse, 1)
	defer close(resChan)

	s.workerPool.QueueExecutable(&RunRequest{
		Path:            path,
		Args:            runner_args,
		ResponseChannel: resChan,
	})

	res := <-resChan

	if res.Err != nil {
		return nil, res.Err
	}

	if res.Status == pb.RunStatus_SUCCESS {
		s.tasksPassed++
	} else {
		s.tasksFailed++
	}

	return res, nil
}

// Serve starts the gRPC server on its configured port. Bind must have been
// called before this; an error is returned if it is not. This function blocks
// until the server is terminated.
func (s *Server) Serve() error {
	if s.listener == nil {
		return errServerNotBound
	}

	log.Printf("Starting gRPC server on %v\n", s.listener.Addr())

	s.startTime = time.Now()
	s.active = true
	s.workerPool.Start()

	return s.grpcServer.Serve(s.listener)
}

// pwTargetRunnerService implements the pw.target_runner.TargetRunner gRPC
// service.
type pwTargetRunnerService struct {
	server *Server
}

// RunBinary runs a single executable on-device and returns its result.
func (s *pwTargetRunnerService) RunBinary(
	ctx context.Context,
	desc *pb.RunBinaryRequest,
) (*pb.RunBinaryResponse, error) {
	var path string

	switch bin := desc.Binary.(type) {
	case *pb.RunBinaryRequest_FilePath:
		path = bin.FilePath
		break
	case *pb.RunBinaryRequest_TestBinary:
		f, err := os.CreateTemp("", "pw_target_runner_")
		if err != nil {
			return nil, status.Errorf(codes.Internal, "Internal server error: %v", err)
		}

		defer os.Remove(f.Name())

		_, err = f.Write(bin.TestBinary)
		if err != nil {
			return nil, status.Errorf(codes.Internal, "Internal server error: %v", err)
		}

		err = os.Chmod(f.Name(), 0755)
		if err != nil {
			return nil, status.Errorf(codes.Internal, "Internal server error: %v", err)
		}

		path = f.Name()
		break
	default:
		return nil, status.Error(codes.InvalidArgument, "No test path or binary provided")
	}

	runRes, err := s.server.RunBinary(path, desc.RunnerArguments)
	if err != nil {
		return nil, status.Errorf(codes.Internal, "Internal server error: %v", err)
	}

	res := &pb.RunBinaryResponse{
		Result:      runRes.Status,
		QueueTimeNs: uint64(runRes.QueueTime),
		RunTimeNs:   uint64(runRes.RunTime),
		Output:      runRes.Output,
	}
	return res, nil
}

// Status returns information about the server.
func (s *pwTargetRunnerService) Status(
	ctx context.Context,
	_ *pb.Empty,
) (*pb.ServerStatus, error) {
	resp := &pb.ServerStatus{
		UptimeNs:    uint64(time.Since(s.server.startTime)),
		TasksPassed: s.server.tasksPassed,
		TasksFailed: s.server.tasksFailed,
	}

	return resp, nil
}
