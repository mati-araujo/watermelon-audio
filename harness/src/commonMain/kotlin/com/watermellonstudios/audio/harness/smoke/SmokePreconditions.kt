package com.watermellonstudios.audio.harness.smoke

/**
 * REQ-053 S1 — las precondiciones que verifica la APP (D4: cada una donde se ve).
 *
 * La ficha de setup (`scripts/smoke-setup.json`) es la única fuente de qué precondiciones tiene
 * cada plan, de qué pasos bloquea cada una y de su remedio. Acá viven sólo los ids que la app
 * EMITE, que tienen que coincidir con los que la ficha declara con `verificador: app`: lo vigila
 * `smoke-device.sh --self-test`. Si la app no emite la línea, el juez la da `no-verificable`, y
 * eso es BLOQUEADO — nunca cumplida.
 */
object SmokePreconditions {
    /** Panel `captura`: el stream de entrada abrió. */
    const val MIC_OPENS: String = "mic-abre"

    /** Panel `usb`: el permiso USB del harness, con su ventana humana (D13). */
    const val USB_PERMISSION: String = "permiso-usb"
}

/** Lo que la app afirma de una precondición: cumplida o no, y lo que vio. */
class PreconditionReading(val met: Boolean, val evidence: String)

/**
 * `mic-abre`: el stream de entrada abrió. Hace falta las dos cosas: que `start()` haya aceptado Y
 * que el stream haya quedado corriendo — aceptar sólo dice que el pedido se encoló.
 */
fun micOpens(accepted: Boolean, running: Boolean, waitedMs: Long): PreconditionReading =
    PreconditionReading(accepted && running, "aceptado:$accepted,corriendo:$running,espera-ms:$waitedMs")

/**
 * `permiso-usb` al cerrar la ventana del diálogo (D13). Devuelve `null` si no hay nada que
 * afirmar: la ventana venció sin respuesta y UsbManager sigue sin permiso. Ese caso NO emite
 * línea, y el juez lo da HUMANO (un humano que no actuó no es un setup roto).
 *
 * Si el diálogo devolvió algo y UsbManager dice que no hay permiso, NO se decide en ese instante:
 * el diálogo puede seguir abierto, porque un broadcast ajeno con `permission=false` aborta la
 * espera de la librería sin cerrarlo (MINI-040; `smoke-device.sh` manda justamente ese falso).
 * Se espera el resto de la ventana: si el humano acepta, el permiso está (cumplida) y el FAIL de
 * `permiso-falso` se juzga; si no, es una negación (incumplida) y el juez da BLOQUEADO. Decidirlo
 * antes taparía con un BLOQUEADO, para siempre, al broadcast ajeno que abortó.
 *
 * @param answered `connectDevice` devolvió dentro de la ventana.
 * @param remainingMs lo que queda de la ventana humana, medido desde que se pidió el diálogo.
 */
suspend fun usbPermissionAfterDialog(
    answered: Boolean,
    hasPermission: () -> Boolean,
    remainingMs: () -> Long,
    pause: suspend (Long) -> Unit,
): PreconditionReading? {
    if (hasPermission()) {
        return PreconditionReading(true, if (answered) "dialogo:concedido" else "concedido-sin-respuesta-de-connect")
    }
    if (!answered) return null
    while (true) {
        val left = remainingMs()
        if (left <= 0) break
        pause(minOf(left, PERMISSION_POLL_MS))
        if (hasPermission()) return PreconditionReading(true, "negado-y-despues-concedido")
    }
    return PreconditionReading(false, "dialogo:denegado")
}

const val PERMISSION_POLL_MS = 500L
