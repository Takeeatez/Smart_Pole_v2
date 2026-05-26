import 'package:flutter/material.dart';
import 'package:flutter_riverpod/flutter_riverpod.dart';
import 'package:go_router/go_router.dart';
import 'package:lucide_icons/lucide_icons.dart';
import 'package:mobile_scanner/mobile_scanner.dart';

import '../../core/services/provisioning_service.dart';
import '../../core/theme/app_colors.dart';
import '../../core/theme/app_spacing.dart';
import '../../core/theme/app_typography.dart';
import '../../shared/widgets/widgets.dart';

/// QR 코드 스캔 스크린
/// 기기에 부착된 QR 코드를 스캔하여 BLE 기기 정보를 읽어옵니다.
class QRScanScreen extends ConsumerStatefulWidget {
  const QRScanScreen({super.key});

  @override
  ConsumerState<QRScanScreen> createState() => _QRScanScreenState();
}

class _QRScanScreenState extends ConsumerState<QRScanScreen> {
  final MobileScannerController _scannerController = MobileScannerController(
    detectionSpeed: DetectionSpeed.normal,
    facing: CameraFacing.back,
    formats: [BarcodeFormat.qrCode],
  );
  bool _hasScanned = false;

  @override
  void dispose() {
    _scannerController.dispose();
    super.dispose();
  }

  void _onDetect(BarcodeCapture capture) {
    if (_hasScanned) return;

    for (final barcode in capture.barcodes) {
      final rawValue = barcode.rawValue;
      if (rawValue == null || rawValue.isEmpty) continue;

      debugPrint('[QR] 감지된 값: $rawValue');

      // QR 데이터 파싱 시도
      try {
        final qrData = DeviceQrData.fromJson(rawValue);
        if (qrData.name.isNotEmpty) {
          setState(() => _hasScanned = true);
          _scannerController.stop();
          // WiFi 목록 화면으로 이동 → 돌아오면 카메라 재시작
          context.push('/wifi-list', extra: qrData).then((_) {
            if (mounted) {
              setState(() => _hasScanned = false);
              _scannerController.start();
            }
          });
          return;
        }
      } catch (e) {
        debugPrint('[QR] 파싱 실패: $e / 원본: $rawValue');
      }
    }
  }

  @override
  Widget build(BuildContext context) {
    return Scaffold(
      body: Container(
        color: AppColors.background,
        child: SafeArea(
          child: Column(
            children: [
              _buildHeader(context),
              Expanded(
                child: Stack(
                  alignment: Alignment.center,
                  children: [
                    // 카메라 프리뷰
                    ClipRRect(
                      borderRadius: AppSpacing.borderRadiusLg,
                      child: MobileScanner(
                        controller: _scannerController,
                        onDetect: _onDetect,
                      ),
                    ),
                    // 스캔 영역 가이드
                    _buildScanOverlay(),
                  ],
                ),
              ),
              Padding(
                padding: const EdgeInsets.fromLTRB(20, 16, 20, 32),
                child: Column(
                  children: [
                    Text(
                      'QR 코드를 스캔해주세요',
                      style: AppTypography.heading3,
                    ),
                    AppSpacing.gapVSm,
                    Text(
                      '기기에 부착된 QR 코드를\n카메라 영역 안에 위치시켜주세요',
                      style: AppTypography.bodyMedium.copyWith(
                        color: AppColors.textMuted,
                      ),
                      textAlign: TextAlign.center,
                    ),
                    AppSpacing.gapVLg,
                    HelpButton(
                      text: 'QR 코드를 찾을 수 없어요',
                      icon: const Icon(LucideIcons.helpCircle,
                          size: 16, color: AppColors.textMuted),
                      onPressed: () {},
                    ),
                  ],
                ),
              ),
            ],
          ),
        ),
      ),
    );
  }

  Widget _buildScanOverlay() {
    return Container(
      width: 240,
      height: 240,
      decoration: BoxDecoration(
        borderRadius: BorderRadius.circular(20),
        border: Border.all(color: AppColors.blue400, width: 3),
      ),
    );
  }

  Widget _buildHeader(BuildContext context) {
    return Padding(
      padding: const EdgeInsets.fromLTRB(20, 24, 20, 16),
      child: Row(
        mainAxisAlignment: MainAxisAlignment.spaceBetween,
        children: [
          AnimatedIconButton(
            icon: LucideIcons.arrowLeft,
            onTap: () {
              if (context.canPop()) {
                context.pop();
              } else {
                context.go('/');
              }
            },
            backgroundColor: AppColors.blue50,
            activeBackgroundColor: AppColors.blue100,
            iconColor: AppColors.blue500,
            activeIconColor: AppColors.blue600,
          ),
          Text('QR 스캔', style: AppTypography.heading4),
          const SizedBox(width: 36),
        ],
      ),
    );
  }
}
