import 'package:flutter_riverpod/flutter_riverpod.dart';

import '../../core/models/notification_data.dart';
import '../../core/services/fcm_service.dart';
import '../../core/services/notification_service.dart';
import 'iv_status_provider.dart';

// ── 서비스 프로바이더 ──

final notificationServiceProvider = Provider<NotificationService>((ref) {
  return NotificationService();
});

// ── 알림 상태 ──

class NotificationState {
  final List<NotificationData> notifications;
  final int total;
  final int currentPage;
  final bool hasMore;

  const NotificationState({
    required this.notifications,
    required this.total,
    required this.currentPage,
    required this.hasMore,
  });

  NotificationState copyWith({
    List<NotificationData>? notifications,
    int? total,
    int? currentPage,
    bool? hasMore,
  }) {
    return NotificationState(
      notifications: notifications ?? this.notifications,
      total: total ?? this.total,
      currentPage: currentPage ?? this.currentPage,
      hasMore: hasMore ?? this.hasMore,
    );
  }
}

// ── 알림 프로바이더 ──

final notificationProvider =
    AsyncNotifierProvider<NotificationNotifier, NotificationState>(
  NotificationNotifier.new,
);

class NotificationNotifier extends AsyncNotifier<NotificationState> {
  @override
  Future<NotificationState> build() {
    // FCM 포그라운드 메시지 수신 시 자동 새로고침
    FcmService.instance.onMessageReceived = () {
      refresh();
      ref.read(dashboardProvider.notifier).refresh();
    };
    return _fetchPage(1);
  }

  Future<NotificationState> _fetchPage(int page) async {
    final service = ref.read(notificationServiceProvider);
    final result = await service.getNotifications(page: page, size: 50);
    return NotificationState(
      notifications: result.items,
      total: result.total,
      currentPage: result.page,
      hasMore: result.hasMore,
    );
  }

  /// 당겨서 새로고침
  Future<void> refresh() async {
    final newState = await AsyncValue.guard(() => _fetchPage(1));
    if (newState.hasValue) {
      state = newState;
    }
  }

  /// 단건 읽음 처리 (낙관적 업데이트)
  Future<void> markAsRead(int notificationId) async {
    final current = state.valueOrNull;
    if (current == null) return;

    // 낙관적 UI 업데이트
    final updated = current.notifications.map((n) {
      if (n.notificationId == notificationId) return n.copyWith(isRead: true);
      return n;
    }).toList();
    state = AsyncValue.data(current.copyWith(notifications: updated));

    try {
      final service = ref.read(notificationServiceProvider);
      await service.markAsRead(notificationId);
      // 대시보드 배지 동기화
      ref.read(dashboardProvider.notifier).refresh();
    } catch (_) {
      // 실패 시 원복
      state = AsyncValue.data(current);
    }
  }

  /// 전체 읽음 처리
  Future<void> markAllAsRead() async {
    final current = state.valueOrNull;
    if (current == null) return;

    // 낙관적 UI 업데이트
    final updated = current.notifications.map((n) => n.copyWith(isRead: true)).toList();
    state = AsyncValue.data(current.copyWith(notifications: updated));

    try {
      final service = ref.read(notificationServiceProvider);
      await service.markAllAsRead();
      ref.read(dashboardProvider.notifier).refresh();
    } catch (_) {
      state = AsyncValue.data(current);
    }
  }
}
