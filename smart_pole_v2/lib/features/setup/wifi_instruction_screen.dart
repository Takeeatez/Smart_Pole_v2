import 'package:flutter/material.dart';
import 'package:flutter_blue_plus/flutter_blue_plus.dart';
import 'package:flutter_riverpod/flutter_riverpod.dart';
import 'package:go_router/go_router.dart';
import 'package:lucide_icons/lucide_icons.dart';

import '../../core/services/provisioning_service.dart';
import '../../core/theme/app_colors.dart';
import '../../core/theme/app_spacing.dart';
import '../../core/theme/app_typography.dart';
import '../../core/utils/ble_permissions.dart';
import '../../shared/providers/provisioning_provider.dart';
import '../../shared/widgets/widgets.dart';

/// WiFi 네트워크 목록 화면
/// QR 스캔 후 BLE로 ESP32에 연결하고, WiFi 스캔 결과를 표시합니다.
class WiFiInstructionScreen extends ConsumerStatefulWidget {
  final DeviceQrData qrData;

  const WiFiInstructionScreen({super.key, required this.qrData});

  @override
  ConsumerState<WiFiInstructionScreen> createState() =>
      _WiFiInstructionScreenState();
}

class _WiFiInstructionScreenState
    extends ConsumerState<WiFiInstructionScreen> {
  @override
  void initState() {
    super.initState();
    // 화면 진입 시 권한 체크 → 프로비저닝 시작
    WidgetsBinding.instance.addPostFrameCallback((_) async {
      // 이전 BLE 스캔/연결 잔여 상태 정리
      await ref.read(provisioningProvider.notifier).reset();
      await FlutterBluePlus.stopScan();

      final readiness = await ensureBlePermissions();
      if (readiness != BleReadiness.ready) {
        if (mounted) {
          ref.read(provisioningProvider.notifier).setError(
                bleReadinessMessage(readiness),
                failedStep: ProvisioningStep.scanningBle,
              );
        }
        return;
      }
      if (mounted) {
        ref.read(provisioningProvider.notifier).startProvisioning(widget.qrData);
      }
    });
  }

  @override
  Widget build(BuildContext context) {
    final state = ref.watch(provisioningProvider);

    // 성공/연결 중 상태에서 자동 화면 전환
    ref.listen<ProvisioningState>(provisioningProvider, (prev, next) {
      if (next.step == ProvisioningStep.success) {
        context.go('/iv-status');
      } else if (next.step == ProvisioningStep.sendingCredentials ||
                 next.step == ProvisioningStep.waitingWifiConnect ||
                 next.step == ProvisioningStep.registeringDevice) {
        context.push('/connecting');
      }
    });

    // 이미 success 상태면 즉시 이동 (ref.listen보다 먼저 state가 바뀐 경우)
    if (state.step == ProvisioningStep.success) {
      WidgetsBinding.instance.addPostFrameCallback((_) {
        if (mounted) context.go('/iv-status');
      });
    }

    return Scaffold(
      body: Container(
        color: AppColors.background,
        child: SafeArea(
          child: Column(
            children: [
              _buildHeader(context),
              Expanded(
                child: _buildContent(state),
              ),
            ],
          ),
        ),
      ),
    );
  }

  Widget _buildContent(ProvisioningState state) {
    switch (state.step) {
      case ProvisioningStep.scanningBle:
      case ProvisioningStep.connectingBle:
      case ProvisioningStep.bleConnected:
      case ProvisioningStep.scanningWifi:
        return _buildLoadingState(state);
      case ProvisioningStep.wifiListReady:
        return _buildWifiList(state.wifiNetworks);
      case ProvisioningStep.error:
        return _buildErrorState(state.errorMessage ?? '알 수 없는 오류');
      case ProvisioningStep.success:
      case ProvisioningStep.registeringDevice:
      case ProvisioningStep.sendingCredentials:
      case ProvisioningStep.waitingWifiConnect:
        return _buildLoadingState(state);
      default:
        return _buildLoadingState(state);
    }
  }

  Widget _buildLoadingState(ProvisioningState state) {
    String message;
    switch (state.step) {
      case ProvisioningStep.scanningBle:
        message = '기기를 검색하고 있어요...';
        break;
      case ProvisioningStep.connectingBle:
        message = '기기에 연결하고 있어요...';
        break;
      case ProvisioningStep.bleConnected:
      case ProvisioningStep.scanningWifi:
        message = '주변 WiFi를 검색하고 있어요...';
        break;
      case ProvisioningStep.registeringDevice:
        message = '기기를 등록하고 있어요...';
        break;
      default:
        message = '준비 중...';
    }

    return Center(
      child: Column(
        mainAxisAlignment: MainAxisAlignment.center,
        children: [
          AnimatedGlowRings(
            outerSize: 160,
            middleSize: 120,
            innerSize: 80,
            animate: true,
            child: Container(
              width: 56,
              height: 56,
              decoration: BoxDecoration(
                gradient: AppColors.iconBadgeGradient,
                shape: BoxShape.circle,
              ),
              child: const Center(
                child: Icon(
                  LucideIcons.bluetooth,
                  size: 28,
                  color: AppColors.blue500,
                ),
              ),
            ),
          ),
          AppSpacing.gapVXl,
          Text(message, style: AppTypography.bodyLarge),
          AppSpacing.gapVSm,
          Text(
            '잠시만 기다려주세요',
            style: AppTypography.bodyMedium.copyWith(
              color: AppColors.textMuted,
            ),
          ),
          AppSpacing.gapVLg,
          const CircularProgressIndicator(
            color: AppColors.blue500,
            strokeWidth: 3,
          ),
        ],
      ),
    );
  }

  Widget _buildWifiList(List<WifiNetwork> networks) {
    return Column(
      crossAxisAlignment: CrossAxisAlignment.start,
      children: [
        Padding(
          padding: const EdgeInsets.fromLTRB(20, 8, 20, 0),
          child: Column(
            crossAxisAlignment: CrossAxisAlignment.start,
            children: [
              IconBadge(
                icon: const Icon(LucideIcons.wifi),
                size: IconBadgeSize.large,
              ),
              AppSpacing.gapVBase,
              Text('WiFi 선택', style: AppTypography.heading2),
              const SizedBox(height: 5),
              Text(
                '기기가 연결할 WiFi를 선택해주세요',
                style: AppTypography.bodyMedium.copyWith(
                  color: AppColors.textMuted,
                ),
              ),
            ],
          ),
        ),
        AppSpacing.gapVBase,
        Expanded(
          child: networks.isEmpty
              ? _buildEmptyWifiList()
              : ListView.separated(
                  padding: const EdgeInsets.symmetric(horizontal: 20),
                  itemCount: networks.length,
                  separatorBuilder: (_, _) => const Divider(height: 1),
                  itemBuilder: (context, index) {
                    final network = networks[index];
                    return _buildWifiTile(network);
                  },
                ),
        ),
        Padding(
          padding: const EdgeInsets.fromLTRB(20, 12, 20, 32),
          child: Column(
            children: [
              SecondaryButton(
                text: '다시 검색',
                onPressed: () {
                  ref.read(provisioningProvider.notifier).scanWifi();
                },
              ),
            ],
          ),
        ),
      ],
    );
  }

  Widget _buildWifiTile(WifiNetwork network) {
    return ListTile(
      contentPadding: const EdgeInsets.symmetric(horizontal: 4, vertical: 4),
      leading: Icon(
        _getSignalIcon(network.signalLevel),
        color: AppColors.blue500,
        size: 24,
      ),
      title: Text(
        network.ssid,
        style: AppTypography.bodyLarge.copyWith(
          fontWeight: FontWeight.w500,
        ),
      ),
      trailing: network.isSecured
          ? const Icon(LucideIcons.lock, size: 16, color: AppColors.textMuted)
          : null,
      onTap: () {
        ref.read(provisioningProvider.notifier).selectWifi(network.ssid);
        context.push('/wifi-input', extra: network.ssid);
      },
    );
  }

  IconData _getSignalIcon(int level) {
    switch (level) {
      case 3:
        return LucideIcons.wifi;
      case 2:
        return LucideIcons.wifi;
      case 1:
        return LucideIcons.wifi;
      default:
        return LucideIcons.wifiOff;
    }
  }

  Widget _buildEmptyWifiList() {
    return Center(
      child: Column(
        mainAxisAlignment: MainAxisAlignment.center,
        children: [
          const Icon(LucideIcons.wifiOff, size: 48, color: AppColors.textMuted),
          AppSpacing.gapVBase,
          Text(
            '주변에 WiFi가 없어요',
            style: AppTypography.bodyLarge.copyWith(color: AppColors.textMuted),
          ),
          AppSpacing.gapVSm,
          Text(
            'WiFi 라우터가 켜져 있는지 확인해주세요',
            style:
                AppTypography.bodyMedium.copyWith(color: AppColors.textMuted),
          ),
        ],
      ),
    );
  }

  Widget _buildErrorState(String message) {
    return Center(
      child: Padding(
        padding: const EdgeInsets.symmetric(horizontal: 20),
        child: Column(
          mainAxisAlignment: MainAxisAlignment.center,
          children: [
            Container(
              width: 64,
              height: 64,
              decoration: BoxDecoration(
                color: AppColors.destructive.withValues(alpha: 0.1),
                shape: BoxShape.circle,
              ),
              child: const Center(
                child: Icon(
                  LucideIcons.alertTriangle,
                  size: 32,
                  color: AppColors.destructive,
                ),
              ),
            ),
            AppSpacing.gapVLg,
            Text(
              '연결에 실패했어요',
              style: AppTypography.heading3,
            ),
            AppSpacing.gapVSm,
            Text(
              message,
              style: AppTypography.bodyMedium.copyWith(
                color: AppColors.textMuted,
              ),
              textAlign: TextAlign.center,
            ),
            AppSpacing.gapVXl,
            PrimaryButton(
              text: '다시 시도',
              onPressed: () {
                ref
                    .read(provisioningProvider.notifier)
                    .startProvisioning(widget.qrData);
              },
            ),
            const SizedBox(height: 12),
            SecondaryButton(
              text: '처음으로',
              onPressed: () => context.go('/setup-guide'),
            ),
          ],
        ),
      ),
    );
  }

  Widget _buildHeader(BuildContext context) {
    return Padding(
      padding: const EdgeInsets.fromLTRB(20, 24, 20, 16),
      child: Row(
        children: [
          AnimatedIconButton(
            icon: LucideIcons.arrowLeft,
            onTap: () {
              ref.read(provisioningProvider.notifier).reset();
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
        ],
      ),
    );
  }
}
