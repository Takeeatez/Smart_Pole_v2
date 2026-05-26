from __future__ import annotations

import json
from datetime import datetime, timezone

from sqlalchemy import select, delete, func
from sqlalchemy.ext.asyncio import AsyncSession

from app.domain.devices.model import Device
from app.domain.auth.model import PatientDevice
from app.domain.infusion.model import DripSession, SensorLog


class DeviceRepository:
    def __init__(self, db: AsyncSession):
        self.db = db

    async def get_devices_by_patient(self, patient_id: int) -> list[Device]:
        result = await self.db.execute(
            select(Device)
            .join(PatientDevice, PatientDevice.device_id == Device.device_id)
            .where(PatientDevice.patient_id == patient_id)
        )
        return list(result.scalars().all())

    async def find_by_id(self, device_id: int) -> Device | None:
        result = await self.db.execute(select(Device).where(Device.device_id == device_id))
        return result.scalars().first()

    async def find_by_qr_code(self, qr_code_value: str) -> Device | None:
        result = await self.db.execute(
            select(Device).where(Device.qr_code_value == qr_code_value)
        )
        return result.scalars().first()

    async def create_device(self, qr_code_value: str) -> Device:
        # QR JSON에서 name을 파싱하여 device_uid로 사용 (ESP32의 X-Device-Serial과 일치)
        try:
            qr_data = json.loads(qr_code_value)
            device_uid = qr_data.get("name", qr_code_value)
        except (json.JSONDecodeError, TypeError):
            device_uid = qr_code_value

        device = Device(
            device_uid=device_uid,
            device_name="Smart Ringer",
            qr_code_value=qr_code_value,
            network_status="offline",
            last_seen_at=datetime.utcnow(),
        )
        self.db.add(device)
        await self.db.commit()
        await self.db.refresh(device)
        return device

    async def create_patient_device(self, patient_id: int, device_id: int) -> PatientDevice:
        # 같은 기기에 연결된 다른 환자를 모두 disconnected로 변경
        await self.disconnect_other_patients(device_id, patient_id)

        pd = PatientDevice(
            patient_id=patient_id,
            device_id=device_id,
            connection_status="connected",
            connected_at=datetime.utcnow(),
            is_primary=True,
        )
        self.db.add(pd)
        await self.db.commit()
        await self.db.refresh(pd)
        return pd

    async def disconnect_other_patients(self, device_id: int, exclude_patient_id: int) -> None:
        """같은 기기에 연결된 다른 환자들을 disconnected로 변경"""
        from sqlalchemy import update
        await self.db.execute(
            update(PatientDevice)
            .where(
                PatientDevice.device_id == device_id,
                PatientDevice.patient_id != exclude_patient_id,
                PatientDevice.connection_status == "connected",
            )
            .values(
                connection_status="disconnected",
                disconnected_at=datetime.utcnow(),
            )
        )

    async def find_patient_device(self, patient_id: int, device_id: int) -> PatientDevice | None:
        result = await self.db.execute(
            select(PatientDevice).where(
                PatientDevice.patient_id == patient_id,
                PatientDevice.device_id == device_id,
            )
        )
        return result.scalars().first()

    async def get_connection_status(self, patient_id: int, device_id: int) -> str:
        pd = await self.find_patient_device(patient_id, device_id)
        return pd.connection_status if pd else "disconnected"

    async def delete_patient_device(self, patient_id: int, device_id: int) -> bool:
        pd = await self.find_patient_device(patient_id, device_id)
        if pd:
            await self.db.delete(pd)
            await self.db.commit()
            return True
        return False

    async def update_last_seen(self, device_id: int) -> None:
        device = await self.find_by_id(device_id)
        if device:
            device.last_seen_at = datetime.utcnow()
            device.network_status = "online"
            await self.db.commit()

    async def cleanup_device_sessions(self, device_id: int) -> None:
        """기기의 모든 세션과 센서 로그 삭제 (기기 연결 해제 시)"""
        # 세션 ID 목록 조회
        result = await self.db.execute(
            select(DripSession.session_id).where(DripSession.device_id == device_id)
        )
        session_ids = [row[0] for row in result.all()]

        if session_ids:
            # 센서 로그 삭제
            await self.db.execute(
                delete(SensorLog).where(SensorLog.session_id.in_(session_ids))
            )
            # 세션 삭제
            await self.db.execute(
                delete(DripSession).where(DripSession.device_id == device_id)
            )
            await self.db.commit()

    async def count_connected_devices(self, patient_id: int) -> int:
        result = await self.db.execute(
            select(func.count()).select_from(PatientDevice).where(
                PatientDevice.patient_id == patient_id,
                PatientDevice.connection_status == "connected",
            )
        )
        return result.scalar() or 0
