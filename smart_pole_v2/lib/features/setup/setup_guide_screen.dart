import 'package:flutter/material.dart';
import 'package:go_router/go_router.dart';
import 'package:lucide_icons/lucide_icons.dart';

import '../../core/theme/app_colors.dart';
import '../../core/theme/app_typography.dart';
import '../../shared/widgets/widgets.dart';

/// 설정 가이드 스크린
/// 기기 연결 단계 안내
class SetupGuideScreen extends StatelessWidget {
  const SetupGuideScreen({super.key});

  @override
  Widget build(BuildContext context) {
    return Scaffold(
      body: Container(
        color: AppColors.background,
        child: SafeArea(
          child: Column(
            children: [
              // Header
              Padding(
                padding: const EdgeInsets.fromLTRB(20, 20, 20, 0),
                child: Column(
                  children: [
                    Align(
                      alignment: Alignment.centerLeft,
                      child: AnimatedIconButton(
                        icon: LucideIcons.arrowLeft,
                        onTap: () {
                          if (context.canPop()) {
                            context.pop();
                          } else {
                            context.go('/');
                          }
                        },
                        iconColor: AppColors.textSecondary,
                        activeIconColor: AppColors.blue500,
                        activeBackgroundColor: AppColors.blue50,
                      ),
                    ),
                    const SizedBox(height: 24),
                    const IconBadge(
                      icon: Icon(LucideIcons.smartphone, color: AppColors.blue500),
                      size: IconBadgeSize.large,
                    ),
                    const SizedBox(height: 16),
                    Text(
                      '기기 연결 가이드',
                      style: AppTypography.heading2.copyWith(
                        color: AppColors.textPrimary,
                      ),
                    ),
                    const SizedBox(height: 8),
                    Text(
                      '아래 단계를 따라 기기를 연결해주세요',
                      style: AppTypography.bodyMedium.copyWith(
                        color: AppColors.textMuted,
                      ),
                    ),
                  ],
                ),
              ),

              // Steps
              Expanded(
                child: SingleChildScrollView(
                  padding: const EdgeInsets.symmetric(horizontal: 20),
                  child: Column(
                    children: [
                      _buildStep(
                        number: 1,
                        title: '기기 전원 켜기',
                        description: '수액 모니터링 기기의 전원 버튼을 눌러 켜주세요',
                        icon: LucideIcons.power,
                      ),
                      _buildStep(
                        number: 2,
                        title: 'QR 코드 스캔',
                        description: '기기에 부착된 QR 코드를 스캔해주세요',
                        icon: LucideIcons.qrCode,
                      ),
                      _buildStep(
                        number: 3,
                        title: 'Wi-Fi 연결',
                        description: '기기가 연결할 Wi-Fi 정보를 입력해주세요',
                        icon: LucideIcons.wifi,
                      ),
                      _buildStep(
                        number: 4,
                        title: '연결 완료',
                        description: '기기 연결이 완료되면 실시간 모니터링을 시작할 수 있어요',
                        icon: LucideIcons.checkCircle,
                      ),
                      const SizedBox(height: 24),
                    ],
                  ),
                ),
              ),

              // Button
              Padding(
                padding: const EdgeInsets.fromLTRB(20, 0, 20, 32),
                child: Column(
                  children: [
                    PrimaryButton(
                      text: '시작하기',
                      onPressed: () => context.push('/qr-scan'),
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

  Widget _buildStep({
    required int number,
    required String title,
    required String description,
    required IconData icon,
  }) {
    return Padding(
      padding: const EdgeInsets.only(bottom: 16),
      child: AppCard(
        child: Row(
          children: [
            Container(
              width: 40,
              height: 40,
              decoration: BoxDecoration(
                color: AppColors.blue50,
                borderRadius: BorderRadius.circular(12),
              ),
              child: Center(
                child: Icon(icon, size: 20, color: AppColors.blue500),
              ),
            ),
            const SizedBox(width: 16),
            Expanded(
              child: Column(
                crossAxisAlignment: CrossAxisAlignment.start,
                children: [
                  Row(
                    children: [
                      Container(
                        width: 20,
                        height: 20,
                        decoration: BoxDecoration(
                          color: AppColors.blue500,
                          shape: BoxShape.circle,
                        ),
                        child: Center(
                          child: Text(
                            '$number',
                            style: const TextStyle(
                              fontSize: 11,
                              fontWeight: FontWeight.w700,
                              color: Colors.white,
                            ),
                          ),
                        ),
                      ),
                      const SizedBox(width: 8),
                      Text(
                        title,
                        style: AppTypography.bodyMediumBold.copyWith(
                          color: AppColors.textPrimary,
                        ),
                      ),
                    ],
                  ),
                  const SizedBox(height: 4),
                  Text(
                    description,
                    style: AppTypography.caption.copyWith(
                      color: AppColors.textMuted,
                    ),
                  ),
                ],
              ),
            ),
          ],
        ),
      ),
    );
  }
}
