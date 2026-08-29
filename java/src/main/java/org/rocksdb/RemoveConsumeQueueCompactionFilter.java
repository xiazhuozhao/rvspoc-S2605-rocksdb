// Copyright (c) Facebook, Inc. and its affiliates. All Rights Reserved.
// This source code is licensed under both the GPLv2 (found in the
// COPYING file in the root directory) and Apache 2.0 License
// (found in the LICENSE.Apache file in the root directory).

package org.rocksdb;

/**
 * Removes RocketMQ consume-queue entries below a CommitLog physical offset.
 *
 * <p>The consume-queue value begins with an eight-byte, big-endian physical
 * offset. Values with a different layout are preserved.</p>
 */
public final class RemoveConsumeQueueCompactionFilter
    extends AbstractCompactionFilter<Slice> {
  public RemoveConsumeQueueCompactionFilter(final long minPhysicalOffset) {
    super(createNewRemoveConsumeQueueCompactionFilter0(minPhysicalOffset));
  }

  private static native long createNewRemoveConsumeQueueCompactionFilter0(
      long minPhysicalOffset);
}
