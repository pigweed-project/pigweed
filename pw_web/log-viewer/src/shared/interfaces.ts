// Copyright 2023 The Pigweed Authors
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

export interface Field {
  key: string;
  value: string | boolean | number | object;
}

export interface TableColumn {
  fieldName: string;
  characterLength: number;
  manualWidth: number | null;
  isVisible: boolean;
}

export interface LogEntry {
  severity?: Level;
  level?: Level;
  timestamp: Date;
  fields: Field[];
  sourceData?: SourceData;
}

export enum Level {
  DEBUG = 'DEBUG',
  INFO = 'INFO',
  WARNING = 'WARNING',
  ERROR = 'ERROR',
  CRITICAL = 'CRITICAL',
}

export enum Severity {
  DEBUG = 'DEBUG',
  INFO = 'INFO',
  WARNING = 'WARNING',
  ERROR = 'ERROR',
  CRITICAL = 'CRITICAL',
}

export interface LogEntryEvent {
  type: 'log-entry';
  data: LogEntry;
}

// Union type for all log source event types
export type LogSourceEvent = LogEntryEvent; /* | ... */

export interface SourceData {
  id: string;
  name: string;
}
