import 'package:flutter/material.dart';
import 'package:flutter_riverpod/flutter_riverpod.dart';
import 'package:go_router/go_router.dart';
import 'package:lucide_icons/lucide_icons.dart';

import '../../core/theme/app_colors.dart';
import '../../core/theme/app_spacing.dart';
import '../../core/theme/app_typography.dart';
import '../../shared/providers/provisioning_provider.dart';
import '../../shared/widgets/widgets.dart';

/// WiFi 비밀번호 입력 스크린
/// WiFi 목록에서 선택한 SSID에 대한 비밀번호를 입력하고 ESP32에 전송합니다.
class WiFiInputScreen extends ConsumerStatefulWidget {
  final String ssid;

  const WiFiInputScreen({super.key, required this.ssid});

  @override
  ConsumerState<WiFiInputScreen> createState() => _WiFiInputScreenState();
}

class _WiFiInputScreenState extends ConsumerState<WiFiInputScreen> {
  final _passwordController = TextEditingController();
  bool _obscurePassword = true;

  @override
  void dispose() {
    _passwordController.dispose();
    super.dispose();
  }

  void _handleConnect() {
    final password = _passwordController.text;
    // WiFi 자격증명 전송 후 연결 화면으로 이동
    ref
        .read(provisioningProvider.notifier)
        .sendWifiCredentials(widget.ssid, password);
    context.push('/connecting');
  }

  @override
  Widget build(BuildContext context) {
    return Scaffold(
      body: Container(
        color: AppColors.background,
        child: SafeArea(
          child: Column(
            crossAxisAlignment: CrossAxisAlignment.start,
            children: [
              _buildHeader(context),
              Expanded(
                child: SingleChildScrollView(
                  padding: const EdgeInsets.symmetric(horizontal: 20),
                  child: Column(
                    crossAxisAlignment: CrossAxisAlignment.start,
                    children: [
                      IconBadge(
                        icon: const Icon(LucideIcons.wifi),
                        size: IconBadgeSize.large,
                      ),
                      AppSpacing.gapVBase,
                      Text('Wi-Fi 비밀번호 입력', style: AppTypography.heading2),
                      const SizedBox(height: 5),
                      Text(
                        '선택한 WiFi의 비밀번호를 입력해주세요',
                        style: AppTypography.bodyMedium
                            .copyWith(color: AppColors.textMuted),
                      ),
                      AppSpacing.gapVXl,
                      AppCard(
                        padding: AppSpacing.cardPaddingLg,
                        child: Column(
                          children: [
                            // SSID 표시 (읽기전용)
                            AppInput(
                              controller:
                                  TextEditingController(text: widget.ssid),
                              label: 'Wi-Fi 이름 (SSID)',
                              placeholder: '',
                              enabled: false,
                            ),
                            AppSpacing.gapVBase,
                            // 비밀번호 입력
                            AppInput(
                              controller: _passwordController,
                              label: '비밀번호',
                              placeholder: 'Wi-Fi 비밀번호 입력',
                              obscureText: _obscurePassword,
                              suffixIcon: IconButton(
                                icon: Icon(
                                  _obscurePassword
                                      ? LucideIcons.eyeOff
                                      : LucideIcons.eye,
                                  size: 20,
                                  color: AppColors.textMuted,
                                ),
                                onPressed: () {
                                  setState(() =>
                                      _obscurePassword = !_obscurePassword);
                                },
                              ),
                            ),
                          ],
                        ),
                      ),
                    ],
                  ),
                ),
              ),
              Padding(
                padding: const EdgeInsets.fromLTRB(20, 0, 20, 32),
                child: Column(
                  children: [
                    PrimaryButton(
                      text: '연결하기',
                      onPressed: _handleConnect,
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

  Widget _buildHeader(BuildContext context) {
    return Padding(
      padding: const EdgeInsets.fromLTRB(20, 24, 20, 16),
      child: AnimatedIconButton(
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
    );
  }
}
