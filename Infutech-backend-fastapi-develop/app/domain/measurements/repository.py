from __future__ import annotations

from datetime import datetime, timezone

from sqlalchemy import select
from sqlalchemy.ext.asyncio import AsyncSession

from app.domain.infusion.model import DripSession, SensorLog


class MeasurementRepository:
    def __init__(self, db: AsyncSession):
        self.db = db

    async def get_or_create_active_session(self, device_id: int, patient_id: int) -> DripSession:
        result = await self.db.execute(
            select(DripSession).where(
                DripSession.device_id == device_id,
                DripSession.status == "running",
            )
        )
        session = result.scalars().first()
        if session:
            return session

        session = DripSession(
            device_id=device_id,
            patient_id=patient_id,
            status="running",
        )
        self.db.add(session)
        await self.db.commit()
        await self.db.refresh(session)
        return session

    async def create_sensor_log(
        self,
        session_id: int,
        device_id: int,
        weight_g: float | None,
        remaining_ml: float,
        drop_rate: float,
        infusion_status: str,
        measured_at: datetime | None,
    ) -> SensorLog:
        log = SensorLog(
            session_id=session_id,
            device_id=device_id,
            measured_weight_g=weight_g,
            remaining_ml=remaining_ml,
            drop_rate=drop_rate,
            infusion_status=infusion_status,
            measured_at=measured_at or datetime.utcnow(),
        )
        self.db.add(log)
        await self.db.commit()
        await self.db.refresh(log)
        return log

    async def find_patient_id_for_device(self, device_id: int) -> int | None:
        from app.domain.auth.model import PatientDevice
        result = await self.db.execute(
            select(PatientDevice.patient_id).where(
                PatientDevice.device_id == device_id,
                PatientDevice.connection_status == "connected",
            ).order_by(PatientDevice.connected_at.desc()).limit(1)
        )
        return result.scalar()
