// [SHIELD_DEVCORE] Internal: emit 'error' without the EventEmitter
// throw-when-unlistened hazard. Async pollers (session, log tail, proc
// sampler) fire error events from inside promise callbacks; a consumer
// that has not attached an error listener yet must not turn that into an
// unhandled rejection.

import type { EventEmitter } from 'node:events';

export function emitErrorSafe(emitter: EventEmitter, error: Error): void {
  if (emitter.listenerCount('error') > 0) {
    emitter.emit('error', error);
  }
}

export function toError(err: unknown): Error {
  return err instanceof Error ? err : new Error(String(err));
}
