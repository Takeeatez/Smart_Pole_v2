import 'dart:io';

import 'package:firebase_messaging/firebase_messaging.dart';
import 'package:flutter/foundation.dart';

import 'api_client.dart';

/// 백그라운드 메시지 핸들러 (top-level function 필수)
@pragma('vm:entry-point')
Future<void> firebaseMessagingBackgroundHandler(RemoteMessage message) async {
  // 백그라운드에서는 별도 처리 없이 시스템 알림으로 표시됨
  debugPrint('[FCM] 백그라운드 메시지: ${message.notification?.title}');
}

class FcmService {
  FcmService._();
  static final FcmService instance = FcmService._();

  final FirebaseMessaging _messaging = FirebaseMessaging.instance;

  /// 포그라운드 메시지 수신 시 콜백 (notification_provider에서 등록)
  VoidCallback? onMessageReceived;

  /// FCM 초기화: 권한 요청 + 토큰 등록 + 리스너 설정
  Future<void> init() async {
    // 1. 알림 권한 요청
    final settings = await _messaging.requestPermission(
      alert: true,
      badge: true,
      sound: true,
    );
    debugPrint('[FCM] 권한 상태: ${settings.authorizationStatus}');

    if (settings.authorizationStatus == AuthorizationStatus.denied) {
      debugPrint('[FCM] 알림 권한 거부됨');
      return;
    }

    // 2. FCM 토큰 획득 + 서버 등록
    final token = await _messaging.getToken();
    if (token != null) {
      await _registerToken(token);
    }

    // 3. 토큰 갱신 시 자동 재등록
    _messaging.onTokenRefresh.listen(_registerToken);

    // 4. 포그라운드 메시지 리스너
    FirebaseMessaging.onMessage.listen(_handleForegroundMessage);

    // 5. iOS 포그라운드 알림 표시 설정
    if (Platform.isIOS) {
      await _messaging.setForegroundNotificationPresentationOptions(
        alert: true,
        badge: true,
        sound: true,
      );
    }
  }

  /// FCM 토큰을 백엔드에 등록
  Future<void> _registerToken(String token) async {
    debugPrint('[FCM] 토큰 등록: ${token.substring(0, 20)}...');
    try {
      await ApiClient.instance.dio.post(
        '/auth/fcm-token',
        data: {'fcm_token': token},
      );
      debugPrint('[FCM] 토큰 서버 등록 완료');
    } catch (e) {
      debugPrint('[FCM] 토큰 등록 실패: $e');
    }
  }

  /// 포그라운드 메시지 처리
  void _handleForegroundMessage(RemoteMessage message) {
    debugPrint('[FCM] 포그라운드 메시지: ${message.notification?.title}');
    onMessageReceived?.call();
  }
}
