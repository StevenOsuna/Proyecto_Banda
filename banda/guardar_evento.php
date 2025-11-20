<?php
$conn = new mysqli("localhost", "root", "", "banda_inteligente");

if ($conn->connect_error) {
    die("Error de conexión: " . $conn->connect_error);
}

$evento = $_POST['evento'] ?? '';
$tipo_caja = $_POST['tipo_caja'] ?? '';
$contador_final = $_POST['contador_final'] ?? 0;

$stmt = $conn->prepare("INSERT INTO eventos_sistema (evento, tipo_caja, contador_final) VALUES (?, ?, ?)");
$stmt->bind_param("ssi", $evento, $tipo_caja, $contador_final);

if ($stmt->execute()) {
    echo "OK";
} else {
    echo "ERROR";
}

$stmt->close();
$conn->close();
?>
