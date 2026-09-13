#include <Arduino.h>
#include <Adafruit_NeoPixel.h>
#include <LittleFS.h>

// Definição dos Pinos no XIAO RP2040
const int pinoLeituraPWM = 2; // GPIO 2 (Recebe o sinal de 5V do SYS_FAN através do divisor de tensão) D8
const int pinoInjecaoPWM = 3; // GPIO 3 (Injeta o sinal de 3.3V direto no resistor de 510 Ohms) D10
const int pinoTacometro = 4;  // GPIO 4 (Aterra o transistor do tacômetro da placa-mãe) D9
unsigned long tempoAnteriorTaco  = 0;
bool estadoTacometro             = false;

int rpmMinimo = 450;  
int rpmMaximo = 1800; 

// Configurações do PWM de Saída
const int frequenciaSaida = 25000; // 25 kHz (Frequência padrão de ventoinhas de PC)
const int resolucaoBits  = 8;     // Resolução de 8 bits (Valores de 0 a 255)
const int dutyMaximo     = 255;   // Valor máximo para 100% de velocidade
const int dutyMinimo     = 50;    // Trava de segurança: ~20% de velocidade (evita que a fan pare totalmente)
const float zeroRpmCutoff = 5.0;

const int pinoLedPower   = 11; // GPIO 11 é o interruptor de energia do NeoPixel no XIAO
const int pinoLedPixel   = 12; // GPIO 12 é a linha de dados digitais do NeoPixel no XIAO
const int numeroPixels   = 1;  // Possui apenas 1 LED integrado
// Instanciação do LED RGB
Adafruit_NeoPixel pixelRGB(numeroPixels, pinoLedPixel, NEO_GRB + NEO_KHZ800);


// Variáveis para o controle de tempo do Heartbeat (Sem travar o código)
unsigned long tempoAnteriorLed = 0;
float anguloHeartbeat = 0.0;

bool debug = false;

void salvarConfiguracaoArquivo() {
    File arquivoEscrita = LittleFS.open("/curva_fan.txt", "w");
    if (!arquivoEscrita) {
        arquivoEscrita = LittleFS.open("curva_fan.txt", "w");
    }

    if (arquivoEscrita) {
        arquivoEscrita.print("rpm_min:"); arquivoEscrita.println(rpmMinimo);
        arquivoEscrita.print("rpm_max:"); arquivoEscrita.println(rpmMaximo);
        arquivoEscrita.close();
        Serial.println("[SUCESSO] Novas configuracoes gravadas fisicamente na Flash!");
    } else {
        Serial.println("[ERRO] Falha ao tentar salvar o arquivo na Flash.");
    }
}

void carregarConfiguracaoArquivo() {
    if (!LittleFS.begin()) {
        Serial.println("[AVISO] Falha ao iniciar LittleFS. Tentando formatar a memoria...");
        
        if (LittleFS.format()) {
            Serial.println("[SUCESSO] Memoria formatada com sucesso!");
            if (!LittleFS.begin()) {
                Serial.println("[ERRO] Mesmo apos formatar, o LittleFS falhou.");
                return;
            }
        } else {
            Serial.println("[ERRO] Falha critica ao tentar formatar a memoria Flash.");
            return;
        }
    }

    // Abre o arquivo para leitura
    File arquivoConfig = LittleFS.open("/curva_fan.txt", "r");
    if (!arquivoConfig) {
        arquivoConfig = LittleFS.open("curva_fan.txt", "r");
    }

    if (!arquivoConfig) {
        Serial.println("[AVISO] Arquivo não encontrado na Flash. Criando um novo agora...");
        
        // Força a criação do arquivo diretamente no chip
        File arquivoEscrita = LittleFS.open("/curva_fan.txt", "w");
        if (arquivoEscrita) {
            arquivoEscrita.println("rpm_min:450");
            arquivoEscrita.println("rpm_max:1800");
            arquivoEscrita.close();
            Serial.println("[SUCESSO] Arquivo 'curva_fan.txt' gerado fisicamente na Flash.");
        }
        return;
    }

    Serial.println("=== Carregando Configuracoes do Arquivo ===");
    while (arquivoConfig.available()) {
        String linha = arquivoConfig.readStringUntil('\n');
        linha.trim(); // Remove espaços e quebras de linha

        if (linha.startsWith("rpm_min:")) {
            rpmMinimo = linha.substring(8).toInt();
            Serial.print("RPM Minimo configurado: "); 
            Serial.println(rpmMinimo);
        } 
        else if (linha.startsWith("rpm_max:")) {
            rpmMaximo = linha.substring(8).toInt();
            Serial.print("RPM Maximo configurado: "); 
            Serial.println(rpmMaximo);
        }
    }
    arquivoConfig.close();
    Serial.println("==========================================");
}


void setup() {
    // Inicializa a comunicação Serial a 115200 bps
    Serial.begin(115200);
    // Configuração dos modos dos pinos
    pinMode(pinoLeituraPWM, INPUT_PULLUP);
    pinMode(pinoInjecaoPWM, OUTPUT);
    pinMode(pinoTacometro, OUTPUT_OPENDRAIN);
    digitalWrite(pinoTacometro, HIGH);
    
    // Configura o hardware do RP2040 para operar na frequência correta de PC
    analogWriteFreq(frequenciaSaida); 
    analogWriteRange(dutyMaximo);  
    
    // Inicia as ventoinhas em 100% por 2 segundo (Boot Inicial de Segurança)
    analogWrite(pinoInjecaoPWM, 255 - dutyMaximo);
    delay(2000);
    
	// LIGA A ENERGIA DO NEOPIXEL (O segredo do XIAO RP2040)
	pinMode(pinoLedPower, OUTPUT);
	digitalWrite(pinoLedPower, HIGH); 

    // Inicializa o LED RGB interno
    pixelRGB.begin();
    pixelRGB.setBrightness(50); // Limita o brilho em ~20% para não ofuscar os olhos
    pixelRGB.show();
    // Tenta ler o arquivo TXT na inicialização do chip
    carregarConfiguracaoArquivo();

    // --- LOOP DE ACORDADA DA DELL (Gera pulsos estáveis sem travar o processador) ---
    // Faz o tacômetro enviar cravados 1200 RPM estáveis por 3 segundos antes de iniciar o loop.
    // Isso garante que quando o processador da Dell checar a linha no POST, leia uma rotação válida.
    unsigned long tempoBoot = millis();
    while (millis() - tempoBoot < 3000) {
        unsigned long tempoAtualMicros = micros();
        // 15000000 / 1200 RPM = 12500 microssegundos por meio ciclo
        if (tempoAtualMicros - tempoAnteriorTaco >= 12500) {
            tempoAnteriorTaco = tempoAtualMicros;
            estadoTacometro = !estadoTacometro;
            // Alterna apenas o estado lógico. Como o pino está em OUTPUT_OPENDRAIN,
            // LOW aterra a linha da Dell (0V) e HIGH solta a linha para subir via pull-up (3.3V/5V)
            digitalWrite(pinoTacometro, estadoTacometro ? LOW : HIGH);
        }
    }

    Serial.println("\n>>> SISTEMA PRONTO PARA COMANDOS SERIAL <<<");
    Serial.println("Exemplos de comando: 'min:500' ou 'max:1800'");
    Serial.printf("Atualmente MIN %d MAX %d", rpmMinimo, rpmMaximo);
}

void loop() {
    if(debug){
        unsigned long tempoAtual = millis();

        // 1. EFEITO HEARTBEAT (Roda continuamente a cada 20 milissegundos)
        if (tempoAtual - tempoAnteriorLed >= 20) {
            tempoAnteriorLed = tempoAtual;
            
            anguloHeartbeat += 0.03;
            if (anguloHeartbeat > 3.14159) {
                anguloHeartbeat = 0.0; 
            }
            
            int brilhoAtual = sin(anguloHeartbeat) * 255;
            
            // Acende em AZUL pulsante
            pixelRGB.setPixelColor(0, pixelRGB.Color(0, 0, brilhoAtual));
            pixelRGB.show();
        }

        // 2. LEITURA DA PORTA SERIAL (Controle das Ventoinhas)
        if (Serial.available() > 0) {
            int valorDigitado = Serial.parseInt();
            
            if (Serial.read() == '\n' || Serial.read() == '\r') {
                // Limpa buffer residual
            }
            
            if (valorDigitado >= 0 && valorDigitado <= 255) {
                Serial.print("Injetando PWM: ");
                Serial.print(valorDigitado);
                Serial.print(" (");
                Serial.print((float)valorDigitado / 255.0 * 100.0, 1);
                Serial.println("%)");
                
                // Pisca em VERMELHO para confirmar o comando
                pixelRGB.setPixelColor(0, pixelRGB.Color(255, 0, 0));
                pixelRGB.show();
                delay(80); 
                
                // Aplica o valor no Gate do MOSFET 20N03
                analogWrite(pinoInjecaoPWM, 255 - valorDigitado);
            } else {
                if(valorDigitado == 256){
                    debug = false;
                    Serial.println("\n>>> SISTEMA PRONTO PARA COMANDOS SERIAL <<<");
                    Serial.println("Exemplos de comando: 'min:500' ou 'max:1800'");
                    Serial.printf("Atualmente MIN %d MAX %d", rpmMinimo, rpmMaximo);
                }
                else
                    Serial.println("[ERRO] Digite um numero valido entre 0 e 255.");
            }
        }
    }else{
  
        unsigned long tempoAtual = millis();
        unsigned long tempoAtualMicros = micros();

        // 1. PROCESSAMENTO DE NOVOS COMANDOS VIA MONITOR SERIAL
        if (Serial.available() > 0) {
            String comandoRecetido = Serial.readStringUntil('\n');
            comandoRecetido.trim(); // Limpa quebras de linha e espacos
            
            if (comandoRecetido.startsWith("min:")) {
                int novoMin = comandoRecetido.substring(4).toInt();
                if (novoMin >= 100 && novoMin < rpmMaximo) {
                    rpmMinimo = novoMin;
                    Serial.print("[OK] Alterando RPM Minimo para: "); Serial.println(rpmMinimo);
                    salvarConfiguracaoArquivo(); // Atualiza o TXT na Flash na hora
                } else {
                    Serial.println("[ERRO] Valor invalido para RPM Minimo.");
                }
            } 
            else if (comandoRecetido.startsWith("max:")) {
                int novoMax = comandoRecetido.substring(4).toInt();
                if (novoMax > rpmMinimo && novoMax <= 5000) {
                    rpmMaximo = novoMax;
                    Serial.print("[OK] Alterando RPM Maximo para: "); 
                    Serial.println(rpmMaximo);
                    salvarConfiguracaoArquivo(); // Atualiza o TXT na Flash na hora
                } else {
                    Serial.println("[ERRO] Valor invalido para RPM Maximo.");
                }
            }else if (comandoRecetido.startsWith("d")){
                debug = true;
                Serial.println("=== MODO DE DEBUG DA CONTROLADORA ===");
                Serial.println("Digite um valor entre 0 e 255 para alterar a velocidade:");
                Serial.println("0   = Totalmente Parado");
                Serial.println("128 = ~50% de Velocidade");
                Serial.println("255 = 100% (Velocidade Maxima)");
                Serial.println("256 = modo normal de funcionamento");
                Serial.println("------------------------------------------------");
            }
        }
        
        if(!debug){
            // 2. LEITURA DO PWM DA PLACA-MÃE (AUTOMAÇÃO TÉRMICA)
            unsigned long tempoAlto  = pulseIn(pinoLeituraPWM, HIGH, 40000); 
            unsigned long tempoBaixo = pulseIn(pinoLeituraPWM, LOW, 40000);
            unsigned long tempoTotal = tempoAlto + tempoBaixo;
            bool partida = false;
            
            int valorSaida = dutyMinimo;
            float porcentagemVelocidade = 0.20; 
            float dutyCycleLido = 0.0; // AJUSTE: Declarada aqui em cima para o Print conseguir ler sempre
            
            // Se o pulseIn retornou 0, significa que o sinal está estático (0% ou 100%)
            if (tempoTotal == 0) {
                if (digitalRead(pinoLeituraPWM) == HIGH) {
                    valorSaida = dutyMaximo; // 255 (100% real)
                    dutyCycleLido = 100.0;
                } else {
                    valorSaida = 0; // Desligado
                    dutyCycleLido = 0.0;
                }
                porcentagemVelocidade = (float)valorSaida / dutyMaximo;
            } 
            else {
                dutyCycleLido = ((float)tempoAlto / (float)tempoTotal) * 100.0; // AJUSTE: Agora calcula de 0.0 a 100.0
                int porcentagemLida = (int)dutyCycleLido;
                
                // --- TABELA DE CALIBRAÇÃO FORÇADA DE POTÊNCIA ---
                if (porcentagemLida < zeroRpmCutoff) {
                    valorSaida = 0;   // Modo Zero RPM
                }
                else{
                    // Mapeia linearmente o sinal recebido (5% a 100%) em um fator de 0.0 a 1.0
                    float fatorLinear = (dutyCycleLido - zeroRpmCutoff) / (100.0 - zeroRpmCutoff);
                    if (fatorLinear > 1.0) fatorLinear = 1.0;
                    if (fatorLinear < 0.0) fatorLinear = 0.0;

                    // Calcula o valor real na faixa linear útil redescoberta (50 a 255)
                    valorSaida = dutyMinimo + (fatorLinear * (dutyMaximo - dutyMinimo));
                    
                    // Limitadores físicos rígidos por software
                    if (valorSaida > 255) valorSaida = 255;
                    if (valorSaida < 50)  valorSaida = 50;
                }
                porcentagemVelocidade = (float)valorSaida / dutyMaximo;
            }
            analogWrite(pinoInjecaoPWM, 255 - valorSaida);

            // 3. SIMULAÇÃO DO TACÔMETRO (RPM FALSO BASEADO NO ARQUIVO)
            int rpmAtual = rpmMinimo + (porcentagemVelocidade * (rpmMaximo - rpmMinimo));
            if (rpmAtual < 100) 
                rpmAtual = 100; 
            
            // Se as ventoinhas estiverem completamente desligadas na tabela, zera o tacômetro
            if (valorSaida == 0) rpmAtual = 0;

            // AJUSTE: Movido para logo após o cálculo do rpmAtual e corrigido variáveis do printf
            static unsigned long ultimoPrint = 0;
            if (tempoAtual - ultimoPrint >= 1000) {
                ultimoPrint = tempoAtual;
                Serial.print("Mae: ");
                Serial.print(dutyCycleLido, 1); // Imprime o float com 1 casa decimal
                Serial.print("% | Saida MOSFET: ");
                Serial.print(valorSaida);
                Serial.print(" | Tacometro: ");
                Serial.print(rpmAtual);
                Serial.println(" RPM - 'd' para modo debug");
            }

            if (rpmAtual > 0) {
                unsigned long meioPeriodoTacoMicros = 15000000 / rpmAtual;
                if (tempoAtualMicros - tempoAnteriorTaco >= meioPeriodoTacoMicros) {
                    tempoAnteriorTaco = tempoAtualMicros;
                    estadoTacometro = !estadoTacometro;
                    
                    if (estadoTacometro) {
                        pinMode(pinoTacometro, OUTPUT);
                        digitalWrite(pinoTacometro, LOW);
                    } else {
                        pinMode(pinoTacometro, INPUT);
                    }
                }
            } else {
                pinMode(pinoTacometro, INPUT); // Desliga a linha se o motor parar
            }

            // 4. HEARTBEAT RGB (Fechamento do bloco que havia sido cortado)
            if (tempoAtual - tempoAnteriorLed >= 20) {
                tempoAnteriorLed = tempoAtual;
                anguloHeartbeat += 0.02;
                if (anguloHeartbeat > 3.14159) anguloHeartbeat = 0.0;
                int brilhoAtual = sin(anguloHeartbeat) * 150; // Brilho confortável

                if (valorSaida == 0) {
                    pixelRGB.setPixelColor(0, pixelRGB.Color(brilhoAtual, brilhoAtual, brilhoAtual)); // Branco pulsante se parado
                } else if (dutyCycleLido <= 40.0) {
                    pixelRGB.setPixelColor(0, pixelRGB.Color(0, brilhoAtual, 0)); // Verde se lento
                } else if (dutyCycleLido <= 75.0) {
                    pixelRGB.setPixelColor(0, pixelRGB.Color(0, 0, brilhoAtual)); // Azul se médio
                } else {
                    pixelRGB.setPixelColor(0, pixelRGB.Color(brilhoAtual, 0, 0)); // Vermelho se rápido
                }
                pixelRGB.show();
            }
        }
    }
}
